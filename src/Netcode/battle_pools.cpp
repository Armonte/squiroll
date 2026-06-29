#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "battle_pools.h"
#include "cpp_arena.h"     // trace_alloc — malloc call-site attribution
#include "patch_utils.h"   // _R address literal
#include "util.h"          // thiscall
#include "log.h"

namespace battle_pools {
namespace {

// TF4::TPoolAllocator — 0x1C-byte struct, verified in IDA. Each pool global
// IS this struct, laid out inline in .bss.
struct Pool {
    uint32_t free_head;        // 0x00 free-list head (slot address)
    uint32_t block_list_head;  // 0x04 block-chain head
    uint32_t last_block_size;  // 0x08 byte size of the head block
    uint32_t slot_size;        // 0x0C per-slot stride
    uint32_t grow_count;       // 0x10 slots allocated on next grow (doubles)
    uint32_t grow_count_init;  // 0x14
    uint32_t max_objects;      // 0x18 0 = uncapped
};

// The battle-object TPoolAllocator globals (RVA → runtime via _R), traced
// exhaustively from every TPoolAllocator::Grow (0x37C30) caller in the
// battle code range. Each "New" function ctor references a
// std::_Ref_count_obj_alloc<Class> vtable that pins (pool global → class).
// Excluded on purpose: render pools (0x49B2D0/310), stage-layer pools
// (0x49B710 CompositeSpriteLayerData, 0x49B730 Ring/CylinderLayerData,
// 0x49B750 SpriteLayerData — built once at stage load), and network pools
// (0x49B7B0/7D0/7F0 — the netcode owns those).
// SqFunctionHolder (0x49B630) is the one the dual rollback crash traced to:
// it holds each actor's Squirrel function objects (the stateLabel closure),
// so omitting it left stale {OT_INSTANCE, junk} refs after a rollback.
// Input pools (0x49B450 InputGlobal, 0x49B4B0 InputSingle,
// 0x49B4D0 InputMulti, 0x49B510 InputCommand) — the Manbow input
// subsystem. InputGlobal is the per-player decoded input device
// (Manbow::InputGlobal, 0x128-byte body: x/y/b0..b11/s0..s9 — exactly
// what each actor's Squirrel InputCommand.Update reads as `device`);
// InputCommand (0x214 body) is the per-player command-detector with the
// b0..b5 reservation timers + input-history ring. These were wrongly
// grouped with the network pools — they are per-frame battle state read
// every frame by actor scripts, so they MUST roll back. (Verified via
// the New stubs @0x6D5F0/0x6EA20/0x702A0/0x75480, each of which
// references its std::_Ref_count_obj_alloc<...TPoolAllocator> vtable.)
struct PoolRef { uint32_t rva; const char* name; };
static const PoolRef g_pool_rva[] = {
    { 0x49B370, "Actor2DManager/World2D" },
    { 0x49B390, "Actor2DProcGroup" },
    { 0x49B410, "Camera2D" },
    { 0x49B4F0, "Aura" },
    { 0x49B590, "cEftResChain" },
    { 0x49B5B0, "AnimCtrlTrail" },
    { 0x49B5D0, "AnimCtrlDynamic" },
    { 0x49B5F0, "AnimCtrlStencil" },
    { 0x49B610, "AnimCtrl2D" },
    { 0x49B630, "SqFunctionHolder" },
    { 0x49B650, "AnimCtrl3D" },
    { 0x49B670, "Actor2D" },
    { 0x49B690, "Actor2DGroup" },
    { 0x49B6B0, "Afterimage" },
    { 0x49B6D0, "Sensor" },
    { 0x49B6F0, "Camera3D" },
    { 0x49B770, "ActorCollisionData" },
    { 0x49B790, "EwActor" },
    { 0x49B450, "InputGlobal" },
    { 0x49B4B0, "InputSingle" },
    { 0x49B4D0, "InputMulti" },
    { 0x49B510, "InputCommand" },
};
static constexpr int NPOOL = sizeof(g_pool_rva) / sizeof(g_pool_rva[0]);

static Pool* pool_at(int i) {
    return (Pool*)(g_pool_rva[i].rva + base_address);
}

// Actor-manager region + the second actor-ID counter. The first ID counter
// (0x4DB02C) falls inside the manager region below, so it rides along.
#define ACTOR_MGR_ADDR   (0x4DB020_R)
#define ACTOR_MGR_BYTES  0x40u
#define ACTOR_ID_CTR_B   (0x4DCEE8_R)

// TF4::TPoolAllocator::Grow — __thiscall(this=&pool). Self-contained:
// mallocs a block, threads its slots onto the free list, doubles grow_count.
typedef void thiscall begin_streaming_t(void* pool);
#define begin_streaming ((begin_streaming_t*)(0x37C30_R))

// Walk every block of a pool: fn(block_addr, block_size).
template <typename L>
static void for_each_block(const Pool* p, const L& fn) {
    uint32_t block = p->block_list_head;
    uint32_t size  = p->last_block_size;
    for (int guard = 0; block && guard < 8192; ++guard) {
        fn(block, size);
        uint32_t next      = *(const uint32_t*)(uintptr_t)(block + size - 8);
        uint32_t next_size = *(const uint32_t*)(uintptr_t)(block + size - 4);
        block = next;
        size  = next_size;
    }
}

} // namespace

static uint32_t pool_slots(const Pool* p) {
    if (!p->slot_size) return 0;
    uint32_t slots = 0;
    for_each_block(p, [&](uint32_t, uint32_t s) { slots += (s - 8) / p->slot_size; });
    return slots;
}

void pregrow() {
    // Grow each used pool until it holds >= TARGET slots, so it will not
    // need to grow mid-match (which would change the block set the
    // snapshot walks). 1024 covers a danmaku-heavy peak with headroom;
    // grow_count doubles per call so a couple of grows reach it.
    static constexpr uint32_t TARGET = 1024;
    for (int i = 0; i < NPOOL; ++i) {
        Pool* p = pool_at(i);
        if (p->slot_size == 0) continue;  // pool never used yet — leave it
        for (int g = 0; g < 10 && pool_slots(p) < TARGET; ++g) {
            begin_streaming(p);
        }
    }
    for (int i = 0; i < NPOOL; ++i) {
        Pool* p = pool_at(i);
        uint32_t blocks = 0;
        if (p->slot_size) for_each_block(p, [&](uint32_t, uint32_t) { ++blocks; });
        log_printf("[battle_pools] %-22s slot=%u blocks=%u slots=%u\n",
                   g_pool_rva[i].name, p->slot_size, blocks, pool_slots(p));
    }
}

// Re-home every live AnimationController2D's three CompositeSprite
// std::vector backing buffers into cpp_arena. The controllers are built by
// vs.Initialize before cpp_arena is armed, so their vector buffers sit on
// the CRT heap, uncaptured — a rollback re-sim then runs on stale buffers
// (the 0xB9196 crash). Here we allocate fresh 256-element buffers via
// th155's operator new (0x2E15AB; cpp_arena, now armed, routes them into
// the captured arena), copy the live elements, and repoint the vector
// header. 256 == one past the u8 max sprite count, so the vectors never
// realloc/move for the rest of the match. The old CRT buffer is leaked
// (one-time, a few KB total — far safer than risking a bad free).
void reserve_anim_vectors() {
    Pool* pl = pool_at(8);  // g_pool_rva[8] = AnimCtrl2D
    if (!pl->slot_size) {
        log_printf("[battle_pools] reserve_anim_vectors: AnimCtrl2D pool unused\n");
        return;
    }
    const uint32_t kVtable = (uint32_t)(0x445E10_R);  // Manbow::AnimationController2D
    typedef void* (*opnew_t)(size_t);
    opnew_t op_new = (opnew_t)(0x2E15AB_R);            // operator new (cpp_arena-hooked)
    static const uint32_t VEC_OFF[3] = { 0x88, 0x94, 0xA0 };  // 3 vectors, slot offsets
    static const uint32_t CAP = 256;                  // sprite count is u8 -> <= 255
    int homed = 0, skipped = 0;
    for_each_block(pl, [&](uint32_t blk, uint32_t bsize) {
        uint32_t span = bsize > 8 ? bsize - 8 : 0;
        for (uint32_t off = 0; off + pl->slot_size <= span; off += pl->slot_size) {
            uint32_t slot = blk + off;
            // Live AnimationController2D <=> object vtable at slot+0x10.
            if (*(const uint32_t*)(uintptr_t)(slot + 0x10) != kVtable) continue;
            for (int k = 0; k < 3; ++k) {
                uint32_t* vec   = (uint32_t*)(uintptr_t)(slot + VEC_OFF[k]);
                uint32_t  first = vec[0], last = vec[1];
                uint32_t  count = first ? (last - first) >> 3 : 0;
                if (count > CAP) { ++skipped; continue; }  // implausible — leave it
                void* nb = op_new(CAP * 8);
                if (!nb) { ++skipped; continue; }
                if (count) memcpy(nb, (const void*)(uintptr_t)first, count * 8);
                vec[0] = (uint32_t)(uintptr_t)nb;
                vec[1] = (uint32_t)(uintptr_t)nb + count * 8;
                vec[2] = (uint32_t)(uintptr_t)nb + CAP * 8;
                ++homed;
            }
        }
    });
    log_printf("[battle_pools] reserve_anim_vectors: %d vectors re-homed, %d skipped"
               " (cpp_arena used=%u KB)\n", homed, skipped, cpp_arena::used() / 1024);
}

// Blob layout — LIVE-SLOT serialization. A pool is pre-grown to thousands of
// fixed-size slots but a battle only ever fills a fraction of them; dumping
// every block raw copied ~14 MB of mostly-empty slots per frame. Instead we
// emit only ALLOCATED slots in full; a free slot costs 4 bytes (just its
// free-list link). The block chain links + free-slot bodies are immutable
// post-pregrow / dead, so they need no capture.
//   [magic][npool]
//   per pool:
//     [Pool struct 0x1C]
//     [nblk]   then nblk  x [block-addr][block-size]
//     [nlive]  then nlive x [slot-addr][slot-bytes (slot_size)]
//     [nfree]  then nfree x [slot-addr]            (free list, in order)
//   [ACTOR_MGR_BYTES of the manager region][uint32 id_ctr_b]
static constexpr uint32_t POOL_MAGIC = 0x4C4F4F50;  // 'POOL'

// Free-slot bitmap scratch, reused per pool — one bit per slot. A pregrown
// pool holds a couple thousand slots (SqFunctionHolder peaks ~4064); 64K bits
// is wide headroom even if a pool grows mid-match.
static constexpr uint32_t MAXSLOT = 65536;
static uint8_t g_freebits[MAXSLOT / 8];

uint32_t save(uint8_t* out, uint32_t cap) {
    uint8_t* p   = out;
    uint8_t* end = out + cap;
    auto put = [&](const void* src, uint32_t n) -> bool {
        if (p + n > end) return false;
        memcpy(p, src, n);
        p += n;
        return true;
    };

    uint32_t magic = POOL_MAGIC, npool = NPOOL;
    if (!put(&magic, 4) || !put(&npool, 4)) return 0;

    for (int i = 0; i < NPOOL; ++i) {
        Pool* pl = pool_at(i);
        if (!put(pl, sizeof(Pool))) return 0;
        uint32_t ss = pl->slot_size;

        // Enumerate blocks: base address + cumulative slot index + slot count.
        struct Blk { uint32_t addr, size, base_idx, nslots; };
        Blk blk[32];
        uint32_t nblk = 0, total = 0;
        for_each_block(pl, [&](uint32_t b, uint32_t s) {
            if (nblk >= 32) return;
            uint32_t ns = ss ? (s - 8) / ss : 0;
            blk[nblk] = { b, s, total, ns };
            total += ns;
            ++nblk;
        });
        if (total > MAXSLOT) return 0;

        // Mark every free slot in the bitmap by walking the free list.
        memset(g_freebits, 0, (total + 7) / 8);
        for (uint32_t fa = pl->free_head, guard = 0; fa && guard <= total; ++guard) {
            for (uint32_t b = 0; b < nblk; ++b) {
                if (fa >= blk[b].addr && fa < blk[b].addr + blk[b].nslots * ss) {
                    uint32_t idx = blk[b].base_idx + (fa - blk[b].addr) / ss;
                    g_freebits[idx >> 3] |= (uint8_t)(1u << (idx & 7));
                    break;
                }
            }
            fa = *(const uint32_t*)(uintptr_t)fa;
        }

        // Block table.
        if (!put(&nblk, 4)) return 0;
        for (uint32_t b = 0; b < nblk; ++b)
            if (!put(&blk[b].addr, 4) || !put(&blk[b].size, 4)) return 0;

        // Every allocated slot, in full.
        if (p + 4 > end) return 0;
        uint32_t* nlive = (uint32_t*)p; p += 4;
        uint32_t live = 0;
        for (uint32_t b = 0; b < nblk; ++b) {
            for (uint32_t j = 0; j < blk[b].nslots; ++j) {
                uint32_t idx = blk[b].base_idx + j;
                if (g_freebits[idx >> 3] & (1u << (idx & 7))) continue;  // free
                uint32_t sa = blk[b].addr + j * ss;
                if (!put(&sa, 4) || !put((const void*)(uintptr_t)sa, ss)) return 0;
                ++live;
            }
        }
        *nlive = live;

        // The free list, in allocation order — 4 bytes per free slot.
        if (p + 4 > end) return 0;
        uint32_t* nfree = (uint32_t*)p; p += 4;
        uint32_t freec = 0;
        for (uint32_t fa = pl->free_head, guard = 0; fa && guard <= total; ++guard) {
            if (!put(&fa, 4)) return 0;
            ++freec;
            fa = *(const uint32_t*)(uintptr_t)fa;
        }
        *nfree = freec;
    }

    if (!put((const void*)ACTOR_MGR_ADDR, ACTOR_MGR_BYTES)) return 0;
    uint32_t idb = *(const uint32_t*)ACTOR_ID_CTR_B;
    if (!put(&idb, 4)) return 0;
    return (uint32_t)(p - out);
}

// --- Manbow boost::singleton_pool family (Sqrat::PoolAllocator + layer tasks)
// A whole FAMILY of Manbow boost::singleton_pools (struct in .data at 0x4DCxxx/
// 0x4DDxxx, blocks from g_tf4_mspace at ~0x19Dxxxxx) sit OUTSIDE every snapshot
// arena and are NOT TF4 TPoolAllocators, so g_pool_rva[] above never captured
// them. Their .data STRUCT (free_head/block_list/sizes) IS captured by
// engine_snap, but the BLOCKS were not -- a half-capture that is WORSE than
// none: after a rollback engine_snap restores free_head to frame-N while the
// block free-chunks keep their live (post-frame) link values, so the free list
// is INCONSISTENT. Two symptoms, one root:
//   * SqVector3 (this.va/vf/vfBaria): stale va.x -> the f=15 depth-1 walk-vs-
//     stand divergence ([getcmp] proof in actor2d_log).
//   * The per-frame draw-task pools (TPrimitiveLayer<Sprite/...> tasks, the
//     *Layer task/dyn-task pools, EwLayer::Task, effect_actor): a free chunk
//     that was "free" at frame N is LIVE after rollback (reallocated, holding a
//     vtable ptr); AnimationController2D::SetMotion on a HIT frees a layer task,
//     walks the free list, reads that vtable as a next-link and writes through
//     it -> the deterministic AV at th155+0x7F7D4 (Manbow_SpritePrimitiveLayer_
//     free_to_pool). All pool names verified in the IDB.
// Fix: whole-block snapshot of the SIM-MUTATED pools (the pool struct is in
// .data via engine_snap, so only block CONTENTS are needed). Whole-block (not
// live-slot like save()) also restores the free-chunk links, keeping them
// consistent with engine_snap's restored free_head -> alloc determinism for
// free, no dependence on each pool's (differently-encoded) slot stride.
// boost::pool never frees a block mid-match, so block addresses are match-
// stable -> restore in place by address; a block grown after a save is absent
// from the blob and engine_snap restores the struct to exclude it (harmless).
//
// EXCLUDED on purpose: the build-once *LayerData pools (Sprite/Ring/Cylinder/
// CompositeSprite LayerData @ 0x4DC6B0..0x4DC730 -- stage/character-load layer
// definitions, not per-frame state) and the font/UI pools (FontPool/BitmapFont
// @ 0x4DC790/7B0/0x4DD0A0) which are render-side. The mp [sblob] checksum is a
// safety net: if any captured pool is render-nondeterministic it shows up as a
// forward-vs-resim mp divergence and gets pulled back out.
static const uint32_t g_boostpool_rva[] = {
    // Sqrat math pools (Sqrat::PoolAllocator) -- this.va/vf/vfBaria etc.
    0x4DCCC0,  // g_Manbow_SqVector3_pool          (this.va / vf / vfBaria == X)
    0x4DCCE0,  // g_Manbow_SqIndexVector3_pool
    0x4DCD00,  // g_Manbow_SqMatrix_pool
    // TPrimitiveLayer<T> draw-task pools (built per-frame by the animation tree)
    0x4DC3E0,  // PrimLayer_Sprite_DynTask
    0x4DC400,  // PrimLayer_CompositeSprite_Task
    0x4DC420,  // PrimLayer_Cylinder_Task
    0x4DC440,  // PrimLayer_Ring_Task
    0x4DC460,  // g_Manbow_SpritePrimitiveLayer_pool (the SetMotion-on-hit crash)
    0x4DC480,  // PrimLayer_CompositeSprite_DynTask
    0x4DC4A0,  // PrimLayer_Cylinder_DynTask
    0x4DC4C0,  // PrimLayer_Ring_DynTask
    // ILayer task pools
    0x4DC4F0,  // RectangleLayer_Task
    0x4DC510,  // RingLayer_Task
    0x4DC540,  // CylinderLayer_Task
    0x4DC560,  // ParallelLayer_Task
    0x4DC5F0,  // CompositeRectangleLayer_Task
    0x4DC580,  // TrailLayer_LayerTask
    0x4DC5B0,  // TrailLayer_DynLayerTask
    0x4DC5D0,  // TrailLayer_Task
    0x4DC690,  // CompositeRectangleLayer_DynLayerTask
    0x4DD000,  // RectangleLayer_DynLayerTask
    0x4DD020,  // RingLayer_DynLayerTask
    0x4DD040,  // CylinderLayer_DynLayerTask
    0x4DD060,  // ParallelLayer_DynLayerTask
    0x4DD0D0,  // EwLayer_Task
    // actor-hierarchy + effects (sim state)
    0x4DC630,  // Actor2D_ChildNode (Actor2D::SetParent)
    0x4DC770,  // effect_actor
};
static constexpr int NBOOSTPOOL =
    (int)(sizeof(g_boostpool_rva) / sizeof(g_boostpool_rva[0]));
static constexpr uint32_t BOOSTPOOL_MAGIC = 0x4C4F4F4D;  // 'MOOL'

uint32_t boostpool_save(uint8_t* out, uint32_t cap) {
    uint8_t* p = out;
    uint8_t* end = out + cap;
    auto put = [&](const void* s, uint32_t n) -> bool {
        if (p + n > end) return false;
        memcpy(p, s, n);
        p += n;
        return true;
    };

    uint32_t magic = BOOSTPOOL_MAGIC, npool = (uint32_t)NBOOSTPOOL;
    if (!put(&magic, 4) || !put(&npool, 4)) return 0;

    for (int i = 0; i < NBOOSTPOOL; ++i) {
        const Pool* pl = (const Pool*)(g_boostpool_rva[i] + base_address);
        // Count blocks first (single-threaded save: the set is stable between
        // the two walks) so the reader knows how many block records follow.
        uint32_t nblk = 0;
        for_each_block(pl, [&](uint32_t, uint32_t) { ++nblk; });
        if (!put(&nblk, 4)) return 0;
        bool ok = true;
        for_each_block(pl, [&](uint32_t b, uint32_t s) {
            if (!ok) return;
            // [addr][size][size bytes] -- the whole block, including its
            // 8-byte block-list trailer (next-ptr/next-size, stable pointers).
            if (!put(&b, 4) || !put(&s, 4) ||
                !put((const void*)(uintptr_t)b, s)) ok = false;
        });
        if (!ok) return 0;
    }
    return (uint32_t)(p - out);
}

void boostpool_load(const uint8_t* blob, uint32_t len) {
    if (len < 8) return;
    const uint8_t* p   = blob;
    const uint8_t* end = blob + len;
    auto get = [&](void* d, uint32_t n) -> bool {
        if (p + n > end) return false;
        memcpy(d, p, n);
        p += n;
        return true;
    };

    uint32_t magic = 0, npool = 0;
    if (!get(&magic, 4) || !get(&npool, 4)) return;
    if (magic != BOOSTPOOL_MAGIC || npool != (uint32_t)NBOOSTPOOL) {
        log_printf("[battle_pools] boostpool_load: bad header magic=%08x "
                   "npool=%u\n", magic, npool);
        return;
    }

    for (uint32_t i = 0; i < npool && i < (uint32_t)NBOOSTPOOL; ++i) {
        const Pool* pl = (const Pool*)(g_boostpool_rva[i] + base_address);
        uint32_t nblk = 0;
        if (!get(&nblk, 4)) return;
        for (uint32_t b = 0; b < nblk; ++b) {
            uint32_t addr = 0, size = 0;
            if (!get(&addr, 4) || !get(&size, 4)) return;
            if (p + size > end) return;  // truncated blob guard
            // Restore only to a CURRENTLY-VALID block of THIS pool (match-
            // stable, but guard against a malformed blob writing arbitrary
            // memory). Blocks are never freed mid-match, so a saved block is
            // always still present; growth only adds blocks (absent here).
            bool valid = false;
            for_each_block(pl, [&](uint32_t cb, uint32_t cs) {
                if (cb == addr && cs == size) valid = true;
            });
            if (valid) memcpy((void*)(uintptr_t)addr, p, size);
            p += size;
        }
    }
}

void log_fingerprint(const char* tag) {
    for (int i = 0; i < NPOOL; ++i) {
        const Pool* p = pool_at(i);
        uint32_t h = 2166136261u;  // FNV-1a over every block of the pool
        for_each_block(p, [&](uint32_t b, uint32_t s) {
            const uint8_t* d = (const uint8_t*)(uintptr_t)b;
            for (uint32_t k = 0; k < s; ++k) { h ^= d[k]; h *= 16777619u; }
        });
        log_printf("[bpfp] %-9s %-22s %08x\n", tag, g_pool_rva[i].name, h);
    }
}

void load(const uint8_t* blob, uint32_t len) {
    if (len < 8) return;
    const uint8_t* p   = blob;
    const uint8_t* end = blob + len;
    auto get = [&](void* dst, uint32_t n) -> bool {
        if (p + n > end) return false;
        memcpy(dst, p, n);
        p += n;
        return true;
    };
    auto get_u32 = [&](uint32_t& v) -> bool { return get(&v, 4); };

    uint32_t magic = 0, npool = 0;
    get_u32(magic); get_u32(npool);
    if (magic != POOL_MAGIC || npool != (uint32_t)NPOOL) {
        log_printf("[battle_pools] load: bad header magic=%08x npool=%u\n",
                   magic, npool);
        return;
    }

    for (int i = 0; i < NPOOL; ++i) {
        Pool saved;
        if (!get(&saved, sizeof(Pool))) return;
        uint32_t ss = saved.slot_size;

        // Block table — addresses are match-stable (pre-grown, never freed);
        // read past it (validation only — restore is by absolute slot addr).
        uint32_t nblk = 0;
        if (!get_u32(nblk)) return;
        for (uint32_t b = 0; b < nblk; ++b) {
            uint32_t a = 0, s = 0;
            if (!get_u32(a) || !get_u32(s)) return;
        }

        // Live slots — memcpy each back to its stable address.
        uint32_t nlive = 0;
        if (!get_u32(nlive)) return;
        for (uint32_t k = 0; k < nlive; ++k) {
            uint32_t sa = 0;
            if (!get_u32(sa)) return;
            if (p + ss > end) return;
            memcpy((void*)(uintptr_t)sa, p, ss);
            p += ss;
        }

        // Free list — relink each free slot's first dword to the next, so the
        // allocator hands out slots in the exact saved order on re-sim.
        uint32_t nfree = 0;
        if (!get_u32(nfree)) return;
        uint32_t prev = 0;
        for (uint32_t k = 0; k < nfree; ++k) {
            uint32_t fa = 0;
            if (!get_u32(fa)) return;
            if (prev) *(uint32_t*)(uintptr_t)prev = fa;
            prev = fa;
        }
        if (prev) *(uint32_t*)(uintptr_t)prev = 0;

        // Restore the allocator struct (free_head, chain, counters) last.
        *pool_at(i) = saved;
    }

    uint8_t mgr[ACTOR_MGR_BYTES];
    if (!get(mgr, ACTOR_MGR_BYTES)) return;
    memcpy((void*)ACTOR_MGR_ADDR, mgr, ACTOR_MGR_BYTES);
    uint32_t idb = 0;
    if (!get_u32(idb)) return;
    *(uint32_t*)ACTOR_ID_CTR_B = idb;

    // DIAGNOSTIC: re-serialise from the just-restored pools and compare to the
    // blob — a mismatch means save/load is not a faithful round-trip. Capped;
    // the blob is now ~0.4 MB so this is cheap.
    static int bp_selftest = 24;
    if (bp_selftest > 0) {
        --bp_selftest;
        static constexpr uint32_t RE_CAP = 24u * 1024 * 1024;
        static uint8_t* re = nullptr;
        if (!re) re = (uint8_t*)VirtualAlloc(nullptr, RE_CAP,
                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (re) {
            uint32_t rn = save(re, RE_CAP);
            if (rn != len || memcmp(re, blob, len) != 0) {
                uint32_t d = 0, m = rn < len ? rn : len;
                while (d < m && re[d] == blob[d]) ++d;
                log_printf("[bp] ROUND-TRIP FAIL len=%u vs %u  first-diff@%u\n",
                           len, rn, d);
            } else {
                log_printf("[bp] round-trip OK (%u bytes)\n", len);
            }
        }
    }
}

// === DIAGNOSTIC: forward-vs-resim per-offset divergence locator ============
// The per-pool fingerprint narrowed bug #2 to three pools (AnimCtrlDynamic,
// AnimCtrl2D, Actor2DGroup). This locates the exact diverging FIELD: it keeps
// the suspect pools' bytes from each forward advance and, on a re-sim of the
// same frame, reports the first byte that differs decoded to pool/slot/offset.
namespace {

// Indices into g_pool_rva of the pools the fingerprint flagged. Currently the
// four input pools — the [bpfp] per-pool checksum showed InputSingle/Multi/
// Command diverge on the first attack frame (InputGlobal stays identical).
static const int g_suspect[]  = { 18, 19, 20, 21 };  // InputGlobal/Single/Multi/Command
static constexpr int NSUSPECT = sizeof(g_suspect) / sizeof(g_suspect[0]);

static constexpr int      DIFF_RING = 10;            // > the 8-frame rollback window
static constexpr uint32_t DIFF_CAP  = 4u * 1024 * 1024;

struct DiffEntry { int frame; uint32_t len; uint8_t* buf; };
static DiffEntry g_diff[DIFF_RING];
static bool      g_diff_init = false;
// Was: g_diff_done — a one-shot gate that stopped diff_locate after the
// first divergence. Removed when chasing the f=15 1-of-8 transient: we
// want EVERY divergent re-sim attributed.

// Serialize just the suspect pools. Per pool: [pool-index 4][Pool 0x1C]
// [nblocks 4] then per block [addr 4][size 4][bytes...].
static uint32_t suspect_serialize(uint8_t* out, uint32_t cap) {
    uint8_t* p = out;
    uint8_t* end = out + cap;
    auto put = [&](const void* s, uint32_t n) -> bool {
        if (p + n > end) return false;
        memcpy(p, s, n);
        p += n;
        return true;
    };
    for (int si = 0; si < NSUSPECT; ++si) {
        int i = g_suspect[si];
        Pool* pl = pool_at(i);
        uint32_t idx = (uint32_t)i;
        if (!put(&idx, 4) || !put(pl, sizeof(Pool))) return 0;
        if (p + 4 > end) return 0;
        uint32_t* nb = (uint32_t*)p;
        p += 4;
        uint32_t cnt = 0;
        bool ok = true;
        for_each_block(pl, [&](uint32_t b, uint32_t s) {
            if (!ok) return;
            if (!put(&b, 4) || !put(&s, 4) ||
                !put((const void*)(uintptr_t)b, s)) { ok = false; return; }
            ++cnt;
        });
        if (!ok) return 0;
        *nb = cnt;
    }
    return (uint32_t)(p - out);
}

// --- hardware write-watchpoint on the diverging field --------------------
// Dr0 is aimed at the field's real address; a VEH logs the EIP of every
// instruction that writes it. That EIP decompiles to the writer — hence the
// owner of the uncaptured 0x0B8E0000 arena — regardless of how it allocates.
static void*    g_veh        = nullptr;
static uint32_t g_watch_addr = 0;
static int      g_watch_hits = 0;

static LONG CALLBACK watch_veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;  // not a DR hit
    c->Dr6 = 0;
    if (g_watch_hits < 24) {
        ++g_watch_hits;
        const uint32_t* f = (const uint32_t*)(uintptr_t)g_watch_addr;
        log_printf("[fieldwatch] write to %08X by EIP=%08X (rva %08X)  now=%08X\n",
                   g_watch_addr, (uint32_t)c->Eip,
                   (uint32_t)(c->Eip - base_address), *f);
        // Scan the stack for th155 code addresses (return addresses) so the
        // call chain above the writer can be reconstructed. th155.exe code is
        // roughly [base+0x1000, base+0x300000).
        const uint32_t* sp = (const uint32_t*)(uintptr_t)c->Esp;
        for (int k = 0; k < 48; ++k) {
            uint32_t v = sp[k];
            uint32_t rva = v - (uint32_t)base_address;
            if (rva >= 0x1000 && rva < 0x300000)
                log_printf("[fieldwatch]   stack[+0x%02X] ret rva=%08X\n",
                           k * 4, rva);
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct ArmReq { HANDLE thread; uint32_t addr; };
static DWORD WINAPI arm_thread(LPVOID p) {
    ArmReq* r = (ArmReq*)p;
    SuspendThread(r->thread);
    CONTEXT c;
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(r->thread, &c)) {
        c.Dr0 = r->addr;
        // Dr7: L0=1 (bit 0); RW0=01 = break-on-write (bits 16-17);
        // LEN0=11 = 4-byte (bits 18-19).
        c.Dr7 = (c.Dr7 & ~0xF0001u) | 1u | (1u << 16) | (3u << 18);
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        SetThreadContext(r->thread, &c);
    }
    ResumeThread(r->thread);
    CloseHandle(r->thread);
    free(r);
    return 0;
}

static void arm_field_watch(uint32_t field_addr) {
    if (g_watch_addr) return;  // arm once
    g_watch_addr = field_addr;
    g_veh = AddVectoredExceptionHandler(1, watch_veh);
    HANDLE self = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
    ArmReq* r = (ArmReq*)malloc(sizeof(ArmReq));
    r->thread = self;
    r->addr   = field_addr;
    CloseHandle(CreateThread(nullptr, 0, arm_thread, r, 0, nullptr));
    log_printf("[fieldwatch] armed Dr0 write-watch on %08X\n", field_addr);
}

// Decode a blob offset to pool name / block / slot / field offset, and dump
// the diverging slot's DWORDs from both the forward and re-sim runs — the
// values themselves (heap pointer vs small int vs refcount) identify the
// field without needing the struct layout up front.
static void diff_decode(const uint8_t* fwd, const uint8_t* re,
                        uint32_t len, uint32_t d) {
    uint32_t off = 0;
    while (off + 4 + sizeof(Pool) + 4 <= len) {
        uint32_t    idx = *(const uint32_t*)(fwd + off);
        const Pool* pl  = (const Pool*)(fwd + off + 4);
        uint32_t    nb  = *(const uint32_t*)(fwd + off + 4 + sizeof(Pool));
        uint32_t    cur = off + 4 + sizeof(Pool) + 4;
        const char* nm  = (idx < (uint32_t)NPOOL) ? g_pool_rva[idx].name : "?";
        for (uint32_t b = 0; b < nb; ++b) {
            uint32_t addr   = *(const uint32_t*)(fwd + cur);
            uint32_t size   = *(const uint32_t*)(fwd + cur + 4);
            uint32_t bytes0 = cur + 8;
            if (d >= off && d < bytes0 + size) {
                if (d < bytes0) {
                    log_printf("[bpdiff] FIRST DIFF pool='%s' in header/Pool-struct "
                               "blob-off=%u\n", nm, d - off);
                    return;
                }
                uint32_t io  = d - bytes0;
                uint32_t ss  = pl->slot_size ? pl->slot_size : 1;
                uint32_t sl  = io / ss;
                uint32_t fo  = io % ss;
                uint32_t s0  = bytes0 + sl * ss;        // slot start in blob
                log_printf("[bpdiff] FIRST DIFF pool='%s' block=%u slot=%u "
                           "field-off=0x%X slot-addr=0x%X (slot_size=0x%X)\n",
                           nm, b, sl, fo, addr + sl * ss, pl->slot_size);
                for (uint32_t k = 0; k < ss && s0 + k + 4 <= bytes0 + size; k += 4) {
                    uint32_t fv = *(const uint32_t*)(fwd + s0 + k);
                    uint32_t rv = *(const uint32_t*)(re  + s0 + k);
                    log_printf("[bpdiff]   +0x%02X  fwd=%08X  resim=%08X %s\n",
                               k, fv, rv, fv != rv ? "<-- DIFF" : "");
                }
                // For each diverging DWORD: classify it as a pointer with
                // VirtualQuery (so a bogus value can't fault), report the
                // region it lives in, and — if the page is readable — dump
                // the pointee's leading DWORDs (the C++ vtable names it).
                auto probe = [](const char* tag, uint32_t k, uint32_t v) {
                    MEMORY_BASIC_INFORMATION mbi;
                    if (VirtualQuery((void*)(uintptr_t)v, &mbi, sizeof(mbi)) == 0) {
                        log_printf("[bpdiff]   %s +0x%02X=%08X  (VirtualQuery failed)\n",
                                   tag, k, v);
                        return;
                    }
                    bool readable = mbi.State == MEM_COMMIT &&
                        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                                        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                        PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY));
                    log_printf("[bpdiff]   %s +0x%02X=%08X  state=%X protect=%X "
                               "region=[%08X+%X]%s\n",
                               tag, k, v, (unsigned)mbi.State, (unsigned)mbi.Protect,
                               (unsigned)(uintptr_t)mbi.AllocationBase,
                               (unsigned)mbi.RegionSize,
                               readable ? "" : "  [NOT READABLE]");
                    uint32_t rend = (uint32_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize);
                    if (readable && v + 16 <= rend && (v & 3) == 0) {
                        const uint32_t* t = (const uint32_t*)(uintptr_t)v;
                        log_printf("[bpdiff]     -> %08X %08X %08X %08X\n",
                                   t[0], t[1], t[2], t[3]);
                    }
                };
                for (uint32_t k = 0; k < ss && s0 + k + 4 <= bytes0 + size; k += 4) {
                    uint32_t fv = *(const uint32_t*)(fwd + s0 + k);
                    uint32_t rv = *(const uint32_t*)(re  + s0 + k);
                    if (fv == rv) continue;
                    probe("resim", k, rv);
                    probe("fwd  ", k, fv);
                    cpp_arena::trace_alloc(rv);
                    cpp_arena::trace_alloc(fv);
                }
                // Aim a HW write-watchpoint at the field's real address so
                // the next write (the divergence recurs every rollback) is
                // attributed to an exact instruction / function.
                arm_field_watch(addr + sl * ss + fo);
                return;
            }
            cur = bytes0 + size;
        }
        off = cur;
    }
    log_printf("[bpdiff] FIRST DIFF blob-off=%u (undecoded)\n", d);
}

} // namespace

void diff_locate(int frame, int rb) {
    // NB: previously g_diff_done was set after the FIRST divergence so we
    // wouldn't spam the log. But the panopticon use-case wants every
    // divergent re-sim attributed — a 1-of-8 transient divergence (the
    // f=15 EC2C/EE1C velocity write) needs each occurrence dumped. The
    // arm_field_watch() side-effect inside diff_decode is itself a single
    // shot (g_watch_addr guard), so the runaway risk is bounded.
    if (frame < 0) return;
    if (!g_diff_init) {
        for (int i = 0; i < DIFF_RING; ++i) {
            g_diff[i].frame = -1;
            g_diff[i].len   = 0;
            g_diff[i].buf   = (uint8_t*)VirtualAlloc(nullptr, DIFF_CAP,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        g_diff_init = true;
    }
    int slot = frame % DIFF_RING;
    DiffEntry& e = g_diff[slot];
    if (!e.buf) return;

    if (rb == 0) {
        uint32_t n = suspect_serialize(e.buf, DIFF_CAP);
        if (n == 0) { e.frame = -1; log_printf("[bpdiff] serialize overflow f=%d\n", frame); return; }
        e.frame = frame;
        e.len   = n;
        return;
    }

    // Re-simulation. Compare against the stored forward run of this frame.
    if (e.frame != frame || e.len == 0) return;
    static uint8_t* re = nullptr;
    if (!re) re = (uint8_t*)VirtualAlloc(nullptr, DIFF_CAP,
                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!re) return;
    uint32_t rn = suspect_serialize(re, DIFF_CAP);
    if (rn == 0) return;
    if (rn == e.len && memcmp(re, e.buf, rn) == 0) return;  // deterministic

    uint32_t d = 0, m = rn < e.len ? rn : e.len;
    while (d < m && re[d] == e.buf[d]) ++d;
    log_printf("[bpdiff] *** NON-DETERMINISM frame=%d  fwd-len=%u resim-len=%u\n",
               frame, e.len, rn);
    diff_decode(e.buf, re, e.len, d);
    // No g_diff_done flip — let every divergent re-sim of this or any
    // later frame surface. (The HW watchpoint inside diff_decode is itself
    // single-shot, so we don't keep re-arming Dr0.)
}

// --- lean LIVE-SLOT bp diff ([bplive]) -------------------------------------
// Diffs the save() blob (live slots only -- exactly what the [sblob] bp
// checksum hashes) forward-vs-resim, so it surfaces the REAL bp divergence
// (the f=24 actor/hitbox state) WITHOUT the free-slot/stale-pointer noise that
// the all-blocks suspect_serialize diff produced. Runs unconditionally (no
// RB_DIAG), frame-gated to the divergence window so the extra save() is cheap,
// and budget-capped. Decodes the first diverging byte to pool / live-slot real
// address / field offset (save() format: [magic][npool] then per pool
// [Pool][nblk][nblk*(addr,size)][nlive][nlive*(addr,slot_bytes)][nfree][...]).
namespace {
// Walk the save() blob (fwd's structure) and report EVERY diverging live slot
// (pool / slot real address / first diverging field + value), capped. Reporting
// all -- not just the first -- shows whether the actor (Actor2D, late in the
// blob) is the root or whether AnimCtrl2D (early in the blob) diverges on its
// own. Requires matching structure (same liveness); a length mismatch is noted
// by the caller and the walk is best-effort.
void bplive_decode(const uint8_t* fwd, const uint8_t* re, uint32_t len) {
    uint32_t off = 8;  // skip [magic][npool]
    int reports = 0;
    for (int i = 0; i < NPOOL && reports < 14; ++i) {
        if (off + sizeof(Pool) + 4 > len) break;
        const Pool* pl = (const Pool*)(fwd + off);
        uint32_t ss = pl->slot_size ? pl->slot_size : 1;
        const char* nm = g_pool_rva[i].name;
        off += sizeof(Pool);
        uint32_t nblk = *(const uint32_t*)(fwd + off); off += 4 + nblk * 8;
        if (off + 4 > len) break;
        uint32_t nlive = *(const uint32_t*)(fwd + off); off += 4;
        int pool_hits = 0;
        for (uint32_t s = 0; s < nlive; ++s) {
            if (off + 4 > len) return;
            uint32_t sa = *(const uint32_t*)(fwd + off);
            uint32_t bytes0 = off + 4;
            if (bytes0 + ss > len) return;
            // Does this slot diverge? And for the FIRST diverging slot of each
            // pool, dump ALL its diverging dwords (so e.g. the actor's vx/vy as
            // well as pos show up, not just the first field).
            bool slot_div = false;
            for (uint32_t k = 0; k + 4 <= ss; k += 4) {
                if (*(const uint32_t*)(fwd + bytes0 + k) !=
                    *(const uint32_t*)(re  + bytes0 + k)) { slot_div = true; break; }
            }
            if (slot_div && reports < 16 && pool_hits < 2) {
                ++pool_hits;
                log_printf("[bplive]   pool='%s' slot=%08X (slot_size=0x%X):\n", nm, sa, ss);
                for (uint32_t k = 0; k + 4 <= ss && reports < 16; k += 4) {
                    uint32_t fv = *(const uint32_t*)(fwd + bytes0 + k);
                    uint32_t rv = *(const uint32_t*)(re  + bytes0 + k);
                    if (fv != rv) {
                        ++reports;
                        log_printf("[bplive]      +0x%02X fwd=%08X resim=%08X\n", k, fv, rv);
                    }
                }
            }
            off = bytes0 + ss;
        }
        if (off + 4 > len) break;
        uint32_t nfree = *(const uint32_t*)(fwd + off); off += 4 + nfree * 4;
    }
}
}  // namespace

void diff_live(int frame, int rb) {
    if (frame < 18 || frame > 45) return;   // the bp divergence window (f=24..31)
    static constexpr int      RING = 14;
    static constexpr uint32_t CAP  = 4u * 1024 * 1024;
    static uint8_t* ring[RING] = {};
    static uint32_t rlen[RING] = {};
    static int      rframe[RING];
    static uint8_t* tmp = nullptr;
    static bool     init = false;
    static int      budget = 40;
    if (!init) { init = true; for (int i = 0; i < RING; ++i) rframe[i] = -1; }
    int slot = ((frame % RING) + RING) % RING;
    if (!ring[slot]) {
        ring[slot] = (uint8_t*)VirtualAlloc(nullptr, CAP,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!ring[slot]) return;
    }
    if (rb == 0) { rlen[slot] = save(ring[slot], CAP); rframe[slot] = frame; return; }
    if (rframe[slot] != frame || rlen[slot] == 0 || budget <= 0) return;
    if (!tmp) {
        tmp = (uint8_t*)VirtualAlloc(nullptr, CAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!tmp) return;
    }
    uint32_t nlen = save(tmp, CAP);
    if (nlen == 0) return;
    if (nlen == rlen[slot] && memcmp(tmp, ring[slot], nlen) == 0) return;  // deterministic
    --budget;
    if (nlen != rlen[slot])
        log_printf("[bplive] f=%d blob LEN differs fwd=%u now=%u (liveness changed)\n",
                   frame, rlen[slot], nlen);
    log_printf("[bplive] *** bp NON-DET f=%d (all diverging live slots) ***\n", frame);
    bplive_decode(ring[slot], tmp, rlen[slot]);
}

} // namespace battle_pools
