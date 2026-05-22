// safetyhook MUST be included before any squiroll header — util.h #defines
// the calling-convention keywords (cdecl/stdcall/...) as attribute macros
// and safetyhook uses those identifiers as method names. (Same ordering
// constraint as sq_arena.cpp / live_actors.cpp.)
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "cpp_arena.h"
#include "patch_utils.h"   // _R address literal
#include "util.h"
#include "log.h"

// operator new(unsigned int) — ??2@YAPAXI@Z, verified in IDA. THE C++
// allocation chokepoint: every `new` expression and every STL container
// allocation funnels here. cdecl, returns the block.
#define OPERATOR_NEW (0x2E15AB_R)

// __free_base — the universal free chokepoint, verified in IDA. Every
// free in the process bottoms out here: `operator delete` (all variants),
// plain C free(), realloc's internal shrink-free. Hooking it and gating
// by address range guarantees no arena pointer can ever reach the real
// heap-free regardless of which delete/free path the engine used. cdecl.
#define FREE_BASE (0x312347_R)

namespace cpp_arena {
namespace {

static constexpr uint32_t ARENA_SIZE = 64u * 1024 * 1024;  // 64 MB
static constexpr int      CLS_MIN_SH = 4;    // 16-byte smallest block
static constexpr int      CLS_MAX_SH = 24;   // 16 MB largest block
static constexpr int      NCLS       = CLS_MAX_SH - CLS_MIN_SH + 1;
static constexpr uint32_t HDR_MAGIC  = 0x42415043;  // 'CPAB'

// 16-byte block header; keeps the payload 16-byte aligned.
struct Hdr {
    uint32_t cls;        // size-class shift, CLS_MIN_SH..CLS_MAX_SH
    uint32_t reqsize;    // requested payload size
    uint32_t next_free;  // free-list link: arena offset of next free block
    uint32_t magic;      // HDR_MAGIC while allocated, 0 while free
};
static_assert(sizeof(Hdr) == 16, "Hdr must be 16 bytes");

// Allocator metadata. Lives at arena offset 0, so a single memcpy of
// [base, base+bump) captures the allocator's own state AND the whole heap.
struct Meta {
    uint32_t bump;             // offset of next fresh block (high-water)
    uint32_t live_bytes;       // currently-handed-out payload bytes
    uint32_t free_off[NCLS];   // per-class free-list head offset (0 = empty)
    uint32_t reserved[8];
};

static uint8_t* g_base      = nullptr;
static Meta*    g_meta      = nullptr;
static bool     g_installed = false;
static uint32_t g_warn_quota = 8;

// Sim-active gate. Thread-local: set on the main thread around the
// battle sim, so only main-thread battle-frame allocations route into
// the arena and other threads (audio, D3D) never touch it. A plain bool
// would mis-route concurrent allocations from other threads into the
// non-locked bump allocator.
static thread_local bool g_sim_active = false;

static SafetyHookInline g_h_new{};
static SafetyHookInline g_h_free{};

static void warn(const char* fmt, uint32_t a, uint32_t b, uint32_t c) {
    if (g_warn_quota == 0) return;
    --g_warn_quota;
    log_printf(fmt, a, b, c);
}

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
    if (sh > CLS_MAX_SH) {
        warn("[cpp_arena] alloc too big: %u bytes\n", (uint32_t)n, 0, 0);
        return nullptr;
    }
    int      ci  = sh - CLS_MIN_SH;
    uint32_t blk = 1u << sh;
    uint32_t off;
    if (g_meta->free_off[ci]) {
        off = g_meta->free_off[ci];
        g_meta->free_off[ci] = ((Hdr*)(g_base + off))->next_free;
    } else {
        if ((uint64_t)g_meta->bump + blk > ARENA_SIZE) {
            warn("[cpp_arena] !! ARENA FULL (bump=%u +%u > cap)\n",
                 g_meta->bump, blk, 0);
            return nullptr;
        }
        off = g_meta->bump;
        g_meta->bump += blk;
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
        warn("[cpp_arena] !! free of bad/double block, magic=%08x\n",
             h->magic, 0, 0);
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
// operator new: while sim-active is set on this thread, route into the
// arena (fall through to the real operator new if the arena is full).
// __free_base: route to arena_free for any pointer inside the arena —
// detected by address range, exact, on every thread.

static void* cdecl hook_new(size_t size) {
    if (g_sim_active) {
        void* p = arena_alloc(size);
        if (p) return p;  // else arena exhausted — fall through to real heap
    }
    return g_h_new.unsafe_ccall<void*>(size);
}

static void cdecl hook_free(void* block) {
    if (in_arena(block)) { arena_free(block); return; }
    g_h_free.unsafe_ccall<void>(block);
}

} // namespace

void install() {
    if (g_installed) return;

    g_base = (uint8_t*)VirtualAlloc(nullptr, ARENA_SIZE,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_base) {
        log_printf("[cpp_arena] !! VirtualAlloc(%u) failed — engine C++ "
                   "stays on CRT heap\n", ARENA_SIZE);
        return;
    }
    g_meta = (Meta*)g_base;
    g_meta->bump       = (sizeof(Meta) + 15u) & ~15u;  // first block 16-aligned
    g_meta->live_bytes = 0;
    for (int i = 0; i < NCLS; ++i) g_meta->free_off[i] = 0;

    g_h_new  = safetyhook::create_inline((void*)OPERATOR_NEW, (void*)hook_new);
    g_h_free = safetyhook::create_inline((void*)FREE_BASE,    (void*)hook_free);

    int ok = g_h_new.enabled() + g_h_free.enabled();
    g_installed = (ok == 2);
    log_printf("[cpp_arena] install: arena=%p %uMB hooks=%d/2\n",
               g_base, ARENA_SIZE / (1024u * 1024u), ok);
}

void set_sim_active(bool on) { g_sim_active = on; }
bool sim_active()            { return g_sim_active; }

uint8_t* base()      { return g_base; }
uint32_t used()      { return g_meta ? g_meta->bump : 0; }
uint32_t capacity()  { return ARENA_SIZE; }
bool     installed() { return g_installed; }
size_t   live_bytes(){ return g_meta ? g_meta->live_bytes : 0; }

uint32_t save(uint8_t* out, uint32_t cap) {
    if (!g_meta) return 0;
    uint32_t n = g_meta->bump;
    if (n > cap) {
        log_printf("[cpp_arena] !! save OVERFLOW: used=%u > cap=%u\n", n, cap);
        return 0;
    }
    memcpy(out, g_base, n);
    return n;
}

bool load(const uint8_t* blob, uint32_t len) {
    if (!g_base || len < sizeof(Meta) || len > ARENA_SIZE) return false;
    memcpy(g_base, blob, len);
    return true;
}

} // namespace cpp_arena
