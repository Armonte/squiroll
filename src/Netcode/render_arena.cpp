// render_arena — see render_arena.h.
//
// A fixed-base VirtualAlloc region with a small segregated-free-list allocator.
// Plugin UI render objects churn every frame (a few dozen small allocations at
// stable sizes), which a size-class free list handles with zero fragmentation
// and no coalescing. The region is placed ABOVE every snapshot arena (sq 0x24M,
// bullet 0x2AM, cpp 0x30M, tf4A 0x3AM, tf4B 0x44M) and is never registered with
// snapshot_ring, so nothing here is ever rolled back.

#include "render_arena.h"

#include <windows.h>
#include <string.h>

#include "log.h"

namespace render_arena {

// Fixed base above tf4B (0x44M + 32M = 0x46M); leave a gap and take 0x48000000.
// 64 MB is far more than any HUD needs but costs only address space until
// touched (we commit lazily per 1 MB chunk).
static constexpr uintptr_t BASE = 0x48000000u;
static constexpr size_t    CAP  = 64u * 1024 * 1024;
static constexpr size_t    COMMIT_CHUNK = 1u * 1024 * 1024;

static uint8_t* g_base   = nullptr;   // reserved region (== BASE on success)
static size_t   g_bump   = 0;         // next fresh byte
static size_t   g_committed = 0;      // bytes committed from base
static size_t   g_live   = 0;         // live (allocated-not-freed) bytes
static CRITICAL_SECTION g_cs;
static bool     g_cs_init = false;

// Every block carries an 8-byte header so free()/realloc() know its size and
// can validate ownership. usable memory starts at header+8.
struct Hdr { uint32_t size; uint32_t magic; };   // size = rounded payload bytes
static constexpr uint32_t MAGIC = 0x52414248u;   // 'RABH'

// Size classes: 16-byte granularity up to 8 KB (512 classes), then a single
// large first-fit free list. Covers essentially all UI allocations in-class.
static constexpr uint32_t SMALL_MAX = 8192;
static constexpr uint32_t GRAN      = 16;
static constexpr int      NCLASS    = SMALL_MAX / GRAN;   // 512
static void* g_free_small[NCLASS];    // singly-linked: *(void**)block = next
static void* g_free_large = nullptr;  // singly-linked; each carries its Hdr

static inline uint32_t round_up(uint32_t n, uint32_t a) { return (n + a - 1) & ~(a - 1); }

// Thread-local scope depth (sim thread is the only allocator that routes here,
// but a plain counter is fine — enter/leave are always on the same thread).
static thread_local int g_scope_depth = 0;

static void ensure_commit(size_t need_end) {
    while (g_committed < need_end && g_committed < CAP) {
        void* r = VirtualAlloc(g_base + g_committed, COMMIT_CHUNK,
                               MEM_COMMIT, PAGE_READWRITE);
        if (!r) { log_printf("[render_arena] commit failed at +0x%zx\n", g_committed); return; }
        g_committed += COMMIT_CHUNK;
    }
}

void init() {
    if (g_base) return;
    if (!g_cs_init) { InitializeCriticalSection(&g_cs); g_cs_init = true; }
    void* r = VirtualAlloc((void*)BASE, CAP, MEM_RESERVE, PAGE_READWRITE);
    if (!r) {
        // Fixed base taken (unlikely, we sit above the arenas) — let the OS pick.
        r = VirtualAlloc(nullptr, CAP, MEM_RESERVE, PAGE_READWRITE);
        if (!r) { log_printf("[render_arena] RESERVE failed\n"); return; }
    }
    g_base = (uint8_t*)r;
    g_bump = 0; g_committed = 0; g_live = 0;
    memset(g_free_small, 0, sizeof(g_free_small));
    g_free_large = nullptr;
    ensure_commit(COMMIT_CHUNK);
    log_printf("[render_arena] ready base=%p cap=0x%zx\n", g_base, CAP);
}

bool ready() { return g_base != nullptr; }

bool owns(const void* p) {
    return g_base && (const uint8_t*)p >= g_base && (const uint8_t*)p < g_base + CAP;
}

static void* alloc_locked(size_t n) {
    if (!g_base) return nullptr;
    uint32_t payload = round_up((uint32_t)(n ? n : 1), GRAN);
    // --- small: size-class free list, else bump -----------------------------
    if (payload <= SMALL_MAX) {
        int cls = (int)(payload / GRAN) - 1;                 // exact class
        if (g_free_small[cls]) {
            void* blk = g_free_small[cls];
            g_free_small[cls] = *(void**)((uint8_t*)blk + sizeof(Hdr));
            g_live += payload;
            return (uint8_t*)blk + sizeof(Hdr);
        }
    } else {
        // --- large: first-fit over the large free list ----------------------
        void** pp = &g_free_large;
        while (*pp) {
            Hdr* h = (Hdr*)*pp;
            if (h->size >= payload) {
                void* blk = *pp;
                *pp = *(void**)((uint8_t*)blk + sizeof(Hdr));
                g_live += ((Hdr*)blk)->size;
                return (uint8_t*)blk + sizeof(Hdr);
            }
            pp = (void**)((uint8_t*)*pp + sizeof(Hdr));
        }
    }
    // Fresh bump.
    size_t need = g_bump + sizeof(Hdr) + payload;
    if (need > CAP) { log_printf("[render_arena] OOM want=%u live=%zu\n", payload, g_live); return nullptr; }
    if (need > g_committed) ensure_commit(need);
    Hdr* h = (Hdr*)(g_base + g_bump);
    h->size = payload; h->magic = MAGIC;
    g_bump = need;
    g_live += payload;
    return (uint8_t*)h + sizeof(Hdr);
}

void* alloc(size_t n) {
    EnterCriticalSection(&g_cs);
    void* r = alloc_locked(n);
    LeaveCriticalSection(&g_cs);
    static int d = 24; if (d > 0) { --d; log_printf("[render_arena] alloc n=%zu -> %p (live=%zu)\n", n, r, g_live); }
    return r;
}

void free(void* p) {
    if (!p || !owns(p)) return;
    EnterCriticalSection(&g_cs);
    Hdr* h = (Hdr*)((uint8_t*)p - sizeof(Hdr));
    if (h->magic != MAGIC) {                 // corruption / double-free guard
        LeaveCriticalSection(&g_cs);
        log_printf("[render_arena] bad free p=%p magic=%08x\n", p, h->magic);
        return;
    }
    uint32_t payload = h->size;
    if (g_live >= payload) g_live -= payload;
    if (payload <= SMALL_MAX) {
        int cls = (int)(payload / GRAN) - 1;
        *(void**)p = g_free_small[cls];      // reuse payload's first word as next
        g_free_small[cls] = h;
    } else {
        *(void**)p = g_free_large;
        g_free_large = h;
    }
    LeaveCriticalSection(&g_cs);
}

void* realloc(void* p, size_t n) {
    if (!p) return alloc(n);
    if (!owns(p)) return nullptr;            // caller must route by owner
    Hdr* h = (Hdr*)((uint8_t*)p - sizeof(Hdr));
    if (h->magic == MAGIC && n <= h->size) return p;   // shrink/fit in place
    void* np = alloc(n);
    if (np && h->magic == MAGIC) memcpy(np, p, h->size < n ? h->size : n);
    free(p);
    return np;
}

void enter() {
    ++g_scope_depth;
    static int d = 8; if (d > 0) { --d; log_printf("[render_arena] enter depth=%d live=%zu\n", g_scope_depth, g_live); }
}
void leave() { if (g_scope_depth > 0) --g_scope_depth; }
bool in_scope() { return g_scope_depth > 0; }

size_t bytes_live()     { return g_live; }
size_t bytes_capacity() { return CAP; }

} // namespace render_arena
