#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "bullet_arena.h"
#include "patch_utils.h"   // _R address literal
#include "util.h"
#include "log.h"

// Bullet's two base allocator function-pointer slots in th155 .data. The
// aligned alloc/free wrappers (0x498D4C/0x498D50) delegate to these, so
// overwriting just this pair captures the entire Bullet heap.
#define BT_ALLOC_FP (0x498D44_R)
#define BT_FREE_FP  (0x498D48_R)

namespace bullet_arena {
namespace {

// 32 MB. Holds th155's live Bullet heap (collision world, broadphase,
// shapes, manifolds, the per-sprite ActorCollisionData hitbox shapes). The
// per-frame snapshot copies only [base, base+bump).
static constexpr uint32_t ARENA_SIZE = 32u * 1024 * 1024;
static constexpr int      CLS_MIN_SH = 4;    // 16-byte smallest block
static constexpr int      CLS_MAX_SH = 24;   // 16 MB largest block
static constexpr int      NCLS       = CLS_MAX_SH - CLS_MIN_SH + 1;
static constexpr uint32_t HDR_MAGIC  = 0x42544142;  // 'BATB' — allocated block
static constexpr uint32_t META_MAGIC = 0x4D544142;  // 'BATM'

// 16-byte block header; keeps the payload 16-byte aligned (Bullet needs it).
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

typedef void* (*bt_alloc_fn)(size_t);
typedef void  (*bt_free_fn)(void*);

static uint8_t*    g_base      = nullptr;
static Meta*       g_meta      = nullptr;
static bool        g_installed = false;
static uint32_t    g_warn      = 8;
static bt_alloc_fn g_orig_alloc = nullptr;
static bt_free_fn  g_orig_free  = nullptr;

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
                log_printf("[bullet_arena] !! ARENA FULL bump=%u +%u\n",
                           g_meta->bump, blk);
            }
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
        if (g_warn) {
            --g_warn;
            log_printf("[bullet_arena] !! free of bad/double block magic=%08x\n",
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

// --- Bullet allocator replacements --------------------------------------

// btAllocFunc: serve from the arena so the Bullet heap is part of the
// snapshot. On overflow fall back to the real allocator (the free hook
// range-routes, so a fallback pointer frees correctly).
static void* cdecl bullet_alloc(size_t size) {
    if (g_meta) {
        void* p = arena_alloc(size);
        if (p) return p;
    }
    return g_orig_alloc ? g_orig_alloc(size) : nullptr;
}

// btFreeFunc: an arena pointer is detected by address range. Everything else
// (a Bullet block allocated before install) goes to the real free.
static void cdecl bullet_free(void* block) {
    if (!block) return;
    if (in_arena(block)) { arena_free(block); return; }
    if (g_orig_free) g_orig_free(block);
}

} // namespace

void install() {
    if (g_installed) return;

    // MEM_WRITE_WATCH: snapshot_ring tracks which pages each frame dirties,
    // so a rollback snapshot copies only what changed, not the whole arena.
    g_base = (uint8_t*)VirtualAlloc(nullptr, ARENA_SIZE,
                                    MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
                                    PAGE_READWRITE);
    if (!g_base) {
        log_printf("[bullet_arena] !! VirtualAlloc(%u) failed — Bullet heap "
                   "stays on the CRT heap\n", ARENA_SIZE);
        return;
    }
    g_meta = (Meta*)g_base;
    g_meta->magic      = META_MAGIC;
    g_meta->bump       = (sizeof(Meta) + 15u) & ~15u;  // first block 16-aligned
    g_meta->live_bytes = 0;
    for (int i = 0; i < NCLS; ++i) g_meta->free_off[i] = 0;

    // Save the original Bullet allocator pointers, then overwrite the base
    // alloc/free slots. The aligned wrappers (0x498D4C/50) delegate here, so
    // this single pair routes every Bullet allocation into the arena.
    void** alloc_fp = (void**)BT_ALLOC_FP;
    void** free_fp  = (void**)BT_FREE_FP;
    g_orig_alloc = (bt_alloc_fn)*alloc_fp;
    g_orig_free  = (bt_free_fn)*free_fp;

    DWORD oldp = 0;
    if (VirtualProtect(alloc_fp, 8, PAGE_READWRITE, &oldp)) {
        *alloc_fp = (void*)bullet_alloc;
        *free_fp  = (void*)bullet_free;
        VirtualProtect(alloc_fp, 8, oldp, &oldp);
        g_installed = true;
    } else {
        log_printf("[bullet_arena] !! VirtualProtect failed — Bullet heap "
                   "stays on the CRT heap\n");
        return;
    }
    log_printf("[bullet_arena] install: arena=%p %uMB orig_alloc=%p "
               "orig_free=%p\n", g_base, ARENA_SIZE / (1024u * 1024u),
               (void*)g_orig_alloc, (void*)g_orig_free);
}

uint8_t* base()      { return g_base; }
uint32_t used()      { return g_meta ? g_meta->bump : 0; }
uint32_t capacity()  { return ARENA_SIZE; }
size_t   live_bytes(){ return g_meta ? g_meta->live_bytes : 0; }

uint32_t save(uint8_t* out, uint32_t cap) {
    if (!g_meta) return 0;
    uint32_t n = g_meta->bump;
    if (n > cap) {
        log_printf("[bullet_arena] !! save OVERFLOW used=%u > cap=%u\n", n, cap);
        return 0;
    }
    memcpy(out, g_base, n);
    return n;
}

void load(const uint8_t* blob, uint32_t len) {
    if (!g_base || len < sizeof(Meta) || len > ARENA_SIZE) return;
    if (((const Meta*)blob)->magic != META_MAGIC) {
        log_printf("[bullet_arena] load: bad magic\n");
        return;
    }
    memcpy(g_base, blob, len);
}

} // namespace bullet_arena
