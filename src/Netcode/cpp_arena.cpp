// safetyhook MUST be included before any squiroll header — util.h #defines
// the calling-convention keywords as attribute macros and safetyhook uses
// those identifiers as method names. (Same ordering constraint as
// sq_arena.cpp.)
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "cpp_arena.h"
#include "patch_utils.h"   // _R address literal
#include "util.h"
#include "log.h"

// operator new(size_t) — th155.exe 0x2E15AB. The single scalar throwing
// operator new: std::vector / std::list (_Buynode0) / std::allocator and
// effectively every C++ heap object th155 allocates routes through it.
// Hooking it captures th155's whole C++ heap into a snapshot-able arena —
// the same model sq_arena uses for the Squirrel heap. (Hooking th155.exe's
// operator new does NOT touch squiroll/GekkoNet allocations: Netcode.dll
// has its own statically-linked CRT, so the arena stays battle-scoped.)
#define OPERATOR_NEW (0x2E15AB_R)
// _free_base — th155.exe 0x312347. The CRT free chokepoint (free / operator
// delete funnel here). Range-routed: an arena pointer goes to arena_free.
#define FREE_BASE    (0x312347_R)
// malloc — th155.exe 0x306FBC. The universal C/C++ heap chokepoint: operator
// new, operator new[] and direct malloc all funnel here. Hooked only to
// record call sites (DIAGNOSTIC) — not routed.
#define MALLOC_FN    (0x306FBC_R)

namespace cpp_arena {
namespace {

// 64 MB. Holds th155's live C++ heap; the per-frame snapshot copies only
// [base, base+bump). Overflow falls back to the real allocator gracefully.
static constexpr uint32_t ARENA_SIZE = 64u * 1024 * 1024;
static constexpr int      CLS_MIN_SH = 4;    // 16-byte smallest block
static constexpr int      CLS_MAX_SH = 24;   // 16 MB largest block
static constexpr int      NCLS       = CLS_MAX_SH - CLS_MIN_SH + 1;
static constexpr uint32_t HDR_MAGIC  = 0x42504143;  // 'CAPB' — allocated block
static constexpr uint32_t META_MAGIC = 0x4D504143;  // 'CAPM'

// 16-byte block header; keeps the payload 16-byte aligned.
struct Hdr {
    uint32_t cls;        // size-class shift, CLS_MIN_SH..CLS_MAX_SH
    uint32_t reqsize;    // requested payload size
    uint32_t next_free;  // free-list link: arena offset of next free block
    uint32_t magic;      // HDR_MAGIC while allocated, 0 while free
};
static_assert(sizeof(Hdr) == 16, "Hdr must be 16 bytes");

// Allocator metadata at arena offset 0 — a single memcpy of [base,bump)
// captures the allocator's own state AND the whole heap, so allocation is
// bit-reproducible across a rollback re-simulation.
struct Meta {
    uint32_t magic;
    uint32_t bump;             // offset of next fresh block (high-water)
    uint32_t live_bytes;       // currently-handed-out payload bytes
    uint32_t free_off[NCLS];   // per-class free-list head offset (0 = empty)
    uint32_t reserved[8];
};

static uint8_t* g_base      = nullptr;
static Meta*    g_meta      = nullptr;
static bool     g_installed = false;
static bool     g_armed     = false;   // route operator new -> arena only while a match is armed
static uint32_t g_warn      = 8;

// DIAGNOSTIC: caller of the in-flight operator new, + a quota for logging
// size-class-13 (8 KB) bump-allocations — the rollback re-sim does one extra,
// diverging cpp_arena (the DESYNC frame=11 root).
static uint32_t g_opnew_caller = 0;
static int      g_cls13_log    = 96;

static SafetyHookInline g_h_opnew{};
static SafetyHookInline g_h_free{};
static SafetyHookInline g_h_malloc{};

// DIAGNOSTIC: ring of recent th155 malloc() calls (caller RVA + size + ptr),
// recorded only while armed. trace_alloc() walks it to attribute a divergent
// pointer to the exact allocation call site.
struct MallocRec { uint32_t caller; uint32_t size; uint32_t ptr; };
static constexpr int MW_RING = 16384;
static MallocRec  g_mw[MW_RING];
static uint32_t   g_mw_idx = 0;

// Smallest class shift whose block (1<<sh) holds sizeof(Hdr)+n.
static int class_for(size_t n) {
    size_t need = n + sizeof(Hdr);
    int sh = CLS_MIN_SH;
    while (((size_t)1 << sh) < need) {
        if (sh >= CLS_MAX_SH) return CLS_MAX_SH + 1;  // too big for the arena
        ++sh;
    }
    return sh;
}

static void* arena_alloc(size_t n) {
    int sh = class_for(n);
    if (sh > CLS_MAX_SH) return nullptr;
    int      ci  = sh - CLS_MIN_SH;
    uint32_t blk = 1u << sh;
    uint32_t off;
    if (g_meta->free_off[ci]) {
        off = g_meta->free_off[ci];
        g_meta->free_off[ci] = ((Hdr*)(g_base + off))->next_free;
    } else {
        if ((uint64_t)g_meta->bump + blk > ARENA_SIZE) {
            if (g_warn) {
                --g_warn;
                log_printf("[cpp_arena] !! ARENA FULL bump=%u +%u\n",
                           g_meta->bump, blk);
            }
            return nullptr;
        }
        off = g_meta->bump;
        g_meta->bump += blk;
        // DIAGNOSTIC: cls-13 (8 KB) bump-grows are the cpp_arena divergence.
        // Scan the stack for th155 return addresses to get the call chain.
        if (sh == 13 && g_cls13_log > 0) {
            --g_cls13_log;
            log_printf("[cpp13] BUMP cls13 size=%u caller_rva=%08X bump->%u "
                       "chain:\n", (uint32_t)n,
                       g_opnew_caller - (uint32_t)base_address, g_meta->bump);
            const uint32_t* sp = (const uint32_t*)&blk;
            for (int k = 0; k < 110; ++k) {
                uint32_t rva = sp[k] - (uint32_t)base_address;
                if (rva >= 0x1000 && rva < 0x300000)
                    log_printf("[cpp13]   stack[+0x%02X] rva=%08X\n", k * 4, rva);
            }
        }
    }
    Hdr* h = (Hdr*)(g_base + off);
    h->cls       = (uint32_t)sh;
    h->reqsize   = (uint32_t)n;
    h->next_free = 0;
    h->magic     = HDR_MAGIC;
    g_meta->live_bytes += (uint32_t)n;
    return g_base + off + sizeof(Hdr);
}

static void arena_free(void* p) {
    Hdr* h = (Hdr*)((uint8_t*)p - sizeof(Hdr));
    if (h->magic != HDR_MAGIC) {
        if (g_warn) {
            --g_warn;
            log_printf("[cpp_arena] !! free of bad/double block magic=%08x\n",
                       h->magic);
        }
        return;
    }
    int ci = (int)h->cls - CLS_MIN_SH;
    if (ci < 0 || ci >= NCLS) return;
    g_meta->live_bytes -= h->reqsize;
    h->magic     = 0;
    h->next_free = g_meta->free_off[ci];
    g_meta->free_off[ci] = (uint32_t)((uint8_t*)h - g_base);
}

static inline bool in_arena(const void* p) {
    return p && (const uint8_t*)p > g_base
             && (const uint8_t*)p < g_base + ARENA_SIZE;
}

// --- hooks --------------------------------------------------------------

// operator new: while a rollback session is armed, serve from the arena so
// the battle's C++ heap is part of the snapshot. Outside a match (boot,
// menus) pass straight to the real allocator — that keeps the arena, hence
// the per-frame snapshot, bounded to battle-era allocations. On overflow
// also fall through (the original handles _callnewh / bad_alloc).
static void* cdecl hook_op_new(size_t size) {
    if (g_armed && g_meta) {
        g_opnew_caller = (uint32_t)(uintptr_t)_ReturnAddress();
        void* p = arena_alloc(size);
        if (p) return p;
    }
    return g_h_opnew.unsafe_ccall<void*>(size);
}

// _free_base: an arena pointer is detected by address range — exact, no
// caller check. Everything else goes to the real CRT free.
static void cdecl hook_free(void* block) {
    if (in_arena(block)) { arena_free(block); return; }
    g_h_free.unsafe_ccall<void>(block);
}

// malloc: pure diagnostic — call the original, then record {caller,size,ptr}
// in the ring while armed. The caller address is malloc's return address
// (safetyhook inline preserves the original call frame).
static void* cdecl hook_malloc(size_t size) {
    void* p = g_h_malloc.unsafe_ccall<void*>(size);
    if (g_armed) {
        uint32_t i = (g_mw_idx++) & (MW_RING - 1);
        g_mw[i].caller = (uint32_t)(uintptr_t)_ReturnAddress();
        g_mw[i].size   = (uint32_t)size;
        g_mw[i].ptr    = (uint32_t)(uintptr_t)p;
    }
    return p;
}

} // namespace

void install() {
    if (g_installed) return;

    g_base = (uint8_t*)VirtualAlloc(nullptr, ARENA_SIZE,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_base) {
        log_printf("[cpp_arena] !! VirtualAlloc(%u) failed — C++ heap stays "
                   "on the CRT heap\n", ARENA_SIZE);
        return;
    }
    g_meta = (Meta*)g_base;
    g_meta->magic      = META_MAGIC;
    g_meta->bump       = (sizeof(Meta) + 15u) & ~15u;  // first block 16-aligned
    g_meta->live_bytes = 0;
    for (int i = 0; i < NCLS; ++i) g_meta->free_off[i] = 0;

    // Install the FREE hook first: the instant the operator-new hook goes
    // live it hands out arena pointers, and their frees must already be
    // range-routed — otherwise a real _free_base on an arena pointer in the
    // install window would corrupt the CRT heap.
    g_h_free   = safetyhook::create_inline((void*)FREE_BASE,    (void*)hook_free);
    g_h_opnew  = safetyhook::create_inline((void*)OPERATOR_NEW, (void*)hook_op_new);
    g_h_malloc = safetyhook::create_inline((void*)MALLOC_FN,    (void*)hook_malloc);

    int ok = g_h_free.enabled() + g_h_opnew.enabled();
    g_installed = (ok == 2);
    log_printf("[cpp_arena] install: arena=%p %uMB hooks=%d/2 mallocwatch=%d\n",
               g_base, ARENA_SIZE / (1024u * 1024u), ok,
               (int)g_h_malloc.enabled());
}

void trace_alloc(uint32_t addr) {
    const MallocRec* best = nullptr;
    for (int k = 0; k < MW_RING; ++k) {
        const MallocRec& r = g_mw[k];
        if (r.ptr && addr >= r.ptr && addr < r.ptr + r.size) {
            // Prefer the tightest enclosing block (the most recent reuse).
            if (!best || r.size < best->size) best = &r;
        }
    }
    if (best) {
        log_printf("[mallocwatch] %08X is inside malloc block [%08X+%X]  "
                   "caller=%08X  rva=%08X\n",
                   addr, best->ptr, best->size, best->caller,
                   (uint32_t)(best->caller - base_address));
    } else {
        log_printf("[mallocwatch] %08X not in malloc ring "
                   "(predates the ring, or not allocated via malloc)\n", addr);
    }
}

void     set_armed(bool on) { g_armed = on; }
uint32_t used()      { return g_meta ? g_meta->bump : 0; }
uint32_t capacity()  { return ARENA_SIZE; }
size_t   live_bytes(){ return g_meta ? g_meta->live_bytes : 0; }

uint32_t save(uint8_t* out, uint32_t cap) {
    if (!g_meta) return 0;
    uint32_t n = g_meta->bump;
    if (n > cap) {
        log_printf("[cpp_arena] !! save OVERFLOW used=%u > cap=%u\n", n, cap);
        return 0;
    }
    memcpy(out, g_base, n);
    return n;
}

void load(const uint8_t* blob, uint32_t len) {
    if (!g_base || len < sizeof(Meta) || len > ARENA_SIZE) return;
    if (((const Meta*)blob)->magic != META_MAGIC) {
        log_printf("[cpp_arena] load: bad magic\n");
        return;
    }
    memcpy(g_base, blob, len);
}

} // namespace cpp_arena
