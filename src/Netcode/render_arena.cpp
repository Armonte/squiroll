// render_arena — see render_arena.h.
//
// A fixed-base VirtualAlloc region with a small segregated-free-list allocator.
// Plugin UI render objects churn every frame (a few dozen small allocations at
// stable sizes), which a size-class free list handles with zero fragmentation
// and no coalescing. The region is placed ABOVE every snapshot arena (sq 0x24M,
// bullet 0x2AM, cpp 0x30M, tf4A 0x3AM, tf4B 0x44M) and is never registered with
// snapshot_ring, so nothing here is ever rolled back.

#include "render_arena.h"

#include <safetyhook.hpp>
#include <windows.h>
#include <string.h>

#include "log.h"
#include "util.h"          // _R literal + thiscall

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

void enter() { ++g_scope_depth; }
void leave() { if (g_scope_depth > 0) --g_scope_depth; }
bool in_scope() { return g_scope_depth > 0; }

// Squirrel plugin sub-domain scope (M3). Separate depth so front-render's
// enter()/leave() (native routing) does NOT reroute its Squirrel allocations.
static thread_local int g_sq_depth = 0;
void sq_enter() { ++g_sq_depth; }
void sq_leave() { if (g_sq_depth > 0) --g_sq_depth; }
bool in_sq_scope() { return g_sq_depth > 0; }

// Plugin bracket: route BOTH native (cpp_arena via in_scope) and Squirrel
// (sq_arena via in_sq_scope) sim-thread allocations into render_arena.
void plugin_scope_enter() { ++g_scope_depth; ++g_sq_depth; }
void plugin_scope_leave() { if (g_sq_depth > 0) --g_sq_depth; if (g_scope_depth > 0) --g_scope_depth; }

size_t bytes_live()     { return g_live; }
size_t bytes_capacity() { return CAP; }

// ---------------------------------------------------------------------------
// UI glyph / BitmapFontResource allocation pinning (task #30, RE by agent).
//
// th155's UI text (Manbow::String / UI.Core.Text) builds its glyph-vertex
// buffer and its Act::BitmapFontResource through operator new during the
// Squirrel .Set()/.ConnectRenderSlot() phase (allocate-on-growth-then-cached,
// so steady-state frames allocate nothing — which is why bracketing the render
// PASS diverted zero bytes). While a match is armed those allocations land in
// the snapshotted arenas; a rollback then rewinds them out from under the async
// D3D driver -> nvwgf2um exec-at-heap. A Manbow::String connected to a render
// slot has TWO forward-only render allocations the driver's draw path touches:
//   (1) its glyph-vertex CPU vector (String+276), grown via
//       String__glyph_vertex_vector_reserve (0x67270) -> operator new; and
//   (2) the TF4::D3D11VertexBuffer control block (String+288) the render closure
//       builds, carved from the g_pool_D3D11VertexBuffer_freelist render pool
//       (0x49B310), whose slabs grow from a snapshotted tf4 mspace via
//       TF4::MeshVertex::PoolAlloc (0x356A0) — NOT operator new. (2) is the
//       object the driver dereferences; its rewound refcount-vtable/type ptr is
//       the crash (confirmed: the 0x67270/operator-new hooks alone don't stop it).
// (1) rides the operator-new scope; (2) needs the render pool's slab carved from
// render_arena. Both render pools (0x49B310 + sibling 0x49B2D0) are 100%
// render-forward-only (engine_snap already excludes their heads). All addresses
// are raw RVAs (IDB base 0).
static SafetyHookInline g_h_glyph_reserve{};
static SafetyHookInline g_h_beginstream{};
static SafetyHookInline g_h_poolalloc{};

// Set (render thread) while growing a render vertex-buffer pool, so the shared
// mspace leaf carves that one slab from render_arena instead.
static thread_local bool g_route_vbpool = false;

// String glyph-vertex CPU buffer (operator new) -> render_arena.
static void* thiscall glyph_reserve_hook(int self, unsigned int new_capacity) {
    Scope s;
    return g_h_glyph_reserve.unsafe_thiscall<void*>(self, new_capacity);
}
// Manbow::NetworkNode::BeginStreaming (0x37C30) grows a pool by one slab. For a
// render vertex-buffer pool, flag the internal mspace alloc for redirection.
static void* thiscall begin_streaming_hook(int self) {
    bool prev = g_route_vbpool;
    uint32_t p = (uint32_t)self;
    if (p == (uint32_t)(0x49B310_R) || p == (uint32_t)(0x49B2D0_R))
        g_route_vbpool = true;
    void* r = g_h_beginstream.unsafe_thiscall<void*>(self);
    g_route_vbpool = prev;
    return r;
}
// TF4::MeshVertex::PoolAlloc (0x356A0, fastcall) — shared tf4 mspace leaf. While
// growing a render pool, carve the slab from render_arena (never snapshotted).
static void* fastcall poolalloc_hook(unsigned int* pool, unsigned int size) {
    if (g_route_vbpool) {
        void* p = alloc(size);
        if (p) return p;   // OOM -> fall through to the real mspace
    }
    return g_h_poolalloc.unsafe_fastcall<void*>(pool, size);
}

void install_ui_hooks() {
    if (!g_base) return;   // region must be reserved first (init())
    g_h_glyph_reserve = safetyhook::create_inline((void*)(0x67270_R), (void*)glyph_reserve_hook);
    g_h_beginstream   = safetyhook::create_inline((void*)(0x37C30_R), (void*)begin_streaming_hook);
    g_h_poolalloc     = safetyhook::create_inline((void*)(0x356A0_R), (void*)poolalloc_hook);
    log_printf("[render_arena] UI render hooks installed: glyph=%d beginstream=%d "
               "poolalloc=%d\n", (int)(bool)g_h_glyph_reserve,
               (int)(bool)g_h_beginstream, (int)(bool)g_h_poolalloc);
}

} // namespace render_arena
