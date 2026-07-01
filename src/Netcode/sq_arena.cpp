// safetyhook MUST be included before any squiroll header — util.h #defines
// the calling-convention keywords (cdecl/stdcall/...) as attribute macros
// and safetyhook uses those identifiers as method names. (Same ordering
// constraint as live_actors.cpp.)
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "sq_arena.h"
#include "patch_utils.h"   // _R address literal
#include "util.h"
#include "log.h"

// The three Squirrel allocator wrapper functions (CRT malloc/realloc/free
// wrappers; verified in IDA). They are COMDAT-folded with non-VM wrappers,
// so the hook routes by the caller's return address: a caller inside the
// Squirrel VM code range gets the arena, everyone else gets the real heap.
#define SQ_MALLOC_BASE  (0x186740_R)
#define SQ_REALLOC_BASE (0x186750_R)
#define SQ_FREE_BASE    (0x186730_R)

namespace sq_arena {
namespace {

static constexpr uint32_t ARENA_SIZE = 64u * 1024 * 1024;  // 64 MB
static constexpr int      CLS_MIN_SH = 4;    // 16-byte smallest block
static constexpr int      CLS_MAX_SH = 24;   // 16 MB largest block
static constexpr int      NCLS       = CLS_MAX_SH - CLS_MIN_SH + 1;
static constexpr uint32_t HDR_MAGIC  = 0x53514142;  // 'SQAB'

// 16-byte block header; keeps the payload 16-byte aligned.
struct Hdr {
    uint32_t cls;        // size-class shift, CLS_MIN_SH..CLS_MAX_SH
    uint32_t reqsize;    // requested payload size (for realloc copy)
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

static uint8_t*  g_base      = nullptr;
static Meta*     g_meta      = nullptr;
static uintptr_t g_sq_lo     = 0;
static uintptr_t g_sq_hi     = 0;
static bool      g_installed = false;
static uint32_t  g_warn_quota = 8;

static SafetyHookInline g_h_malloc{};
static SafetyHookInline g_h_realloc{};
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
        warn("[sq_arena] alloc too big: %u bytes\n", (uint32_t)n, 0, 0);
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
            warn("[sq_arena] !! ARENA FULL (bump=%u +%u > cap)\n",
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
        warn("[sq_arena] !! free of bad/double block, magic=%08x\n",
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

static void* arena_realloc(void* p, size_t n) {
    Hdr* h = (Hdr*)((uint8_t*)p - sizeof(Hdr));
    if (h->magic != HDR_MAGIC) {
        warn("[sq_arena] !! realloc of bad block, magic=%08x\n",
             h->magic, 0, 0);
        return nullptr;
    }
    uint32_t cap = (1u << h->cls) - (uint32_t)sizeof(Hdr);
    if (n <= cap) {
        g_meta->live_bytes += (uint32_t)n - h->reqsize;
        h->reqsize = (uint32_t)n;
        return p;
    }
    void* np = arena_alloc(n);
    if (!np) return nullptr;
    memcpy(np, p, h->reqsize);
    arena_free(p);
    return np;
}

static inline bool in_arena(const void* p) {
    return p && (const uint8_t*)p > g_base
             && (const uint8_t*)p < g_base + ARENA_SIZE;
}
static inline bool from_vm(void* ret) {
    return (uintptr_t)ret >= g_sq_lo && (uintptr_t)ret < g_sq_hi;
}

// --- hooks --------------------------------------------------------------
// Replace sq_malloc_base / sq_realloc_base / sq_free_base. A VM caller
// (return address inside the Squirrel code range) is routed to the arena.
// free/realloc of an arena pointer is detected by address range — exact,
// no caller check needed.

static void* cdecl hook_malloc(size_t size) {
    if (from_vm(__builtin_return_address(0))) {
        void* p = arena_alloc(size);
        if (p) return p;  // else arena exhausted — fall through to real heap
    }
    return g_h_malloc.unsafe_ccall<void*>(size);
}

static void* cdecl hook_realloc(void* block, int oldsize, size_t size) {
    if (in_arena(block)) {
        if (size == 0) { arena_free(block); return nullptr; }
        void* p = arena_realloc(block, size);
        if (p) return p;
        return g_h_realloc.unsafe_ccall<void*>(block, oldsize, size);
    }
    if (!block && from_vm(__builtin_return_address(0))) {
        void* p = arena_alloc(size);
        if (p) return p;
    }
    return g_h_realloc.unsafe_ccall<void*>(block, oldsize, size);
}

static void cdecl hook_free(void* block) {
    if (in_arena(block)) { arena_free(block); return; }
    g_h_free.unsafe_ccall<void>(block);
}

} // namespace

void install(uintptr_t sq_code_lo, uintptr_t sq_code_hi) {
    if (g_installed) return;

    // MEM_WRITE_WATCH: snapshot_ring tracks which pages each frame dirties,
    // so a rollback snapshot copies only what changed, not the whole arena.
    // DETERMINISM: fixed base (see cpp_arena) so sq-object pointers are identical run-to-run.
    g_base = (uint8_t*)VirtualAlloc((void*)0x24000000, ARENA_SIZE,
                                    MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
                                    PAGE_READWRITE);
    if (!g_base)
        g_base = (uint8_t*)VirtualAlloc(nullptr, ARENA_SIZE,
                                        MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
                                        PAGE_READWRITE);
    if (!g_base) {
        log_printf("[sq_arena] !! VirtualAlloc(%u) failed — VM stays on CRT heap\n",
                   ARENA_SIZE);
        return;
    }
    log_printf("[sq_arena] base=%p (fixed-base determinism %s)\n",
               (void*)g_base, ((uintptr_t)g_base == 0x24000000) ? "ON" : "OFF");
    g_meta = (Meta*)g_base;
    g_meta->bump       = (sizeof(Meta) + 15u) & ~15u;  // first block 16-aligned
    g_meta->live_bytes = 0;
    for (int i = 0; i < NCLS; ++i) g_meta->free_off[i] = 0;

    g_sq_lo = sq_code_lo;
    g_sq_hi = sq_code_hi;

    g_h_malloc  = safetyhook::create_inline((void*)SQ_MALLOC_BASE,  (void*)hook_malloc);
    g_h_realloc = safetyhook::create_inline((void*)SQ_REALLOC_BASE, (void*)hook_realloc);
    g_h_free    = safetyhook::create_inline((void*)SQ_FREE_BASE,    (void*)hook_free);

    int ok = g_h_malloc.enabled() + g_h_realloc.enabled() + g_h_free.enabled();
    g_installed = (ok == 3);
    log_printf("[sq_arena] install: arena=%p %uMB hooks=%d/3 sq_code=[%08x,%08x)\n",
               g_base, ARENA_SIZE / (1024u * 1024u), ok,
               (uint32_t)g_sq_lo, (uint32_t)g_sq_hi);
}

uint8_t* base()      { return g_base; }
uint32_t used()      { return g_meta ? g_meta->bump : 0; }
uint32_t capacity()  { return ARENA_SIZE; }
bool     installed() { return g_installed; }
size_t   live_bytes(){ return g_meta ? g_meta->live_bytes : 0; }

uint32_t save(uint8_t* out, uint32_t cap) {
    if (!g_meta) return 0;
    uint32_t n = g_meta->bump;
    if (n > cap) {
        log_printf("[sq_arena] !! save OVERFLOW: used=%u > cap=%u\n", n, cap);
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

} // namespace sq_arena
