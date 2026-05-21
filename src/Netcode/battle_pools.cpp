#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "battle_pools.h"
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

// The 12 known battle-object TPoolAllocator globals (RVA → runtime via _R).
// AnimationControllerStencil's pool is not yet pinned — a rare render
// variant; revisit if a stencil actor desyncs.
struct PoolRef { uint32_t rva; const char* name; };
static const PoolRef g_pool_rva[] = {
    { 0x49B370, "Actor2DManager/World2D" },
    { 0x49B410, "Camera2D" },
    { 0x49B4F0, "Aura" },
    { 0x49B5B0, "AnimCtrlTrail" },
    { 0x49B5D0, "AnimCtrlDynamic" },
    { 0x49B610, "AnimCtrl2D" },
    { 0x49B650, "AnimCtrl3D" },
    { 0x49B670, "Actor2D" },
    { 0x49B690, "Actor2DGroup" },
    { 0x49B6B0, "Afterimage" },
    { 0x49B6D0, "Sensor" },
    { 0x49B6F0, "Camera3D" },
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

// Blob layout:
//   [magic][npool]
//   per pool: [Pool struct 0x1C][nblocks] then nblocks x {addr,size,bytes}
//   [ACTOR_MGR_BYTES of the manager region][uint32 id_ctr_b]
static constexpr uint32_t POOL_MAGIC = 0x4C4F4F50;  // 'POOL'

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
        if (p + 4 > end) return 0;
        uint32_t* nb = (uint32_t*)p;  // patched after the block loop
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

    if (!put((const void*)ACTOR_MGR_ADDR, ACTOR_MGR_BYTES)) return 0;
    uint32_t idb = *(const uint32_t*)ACTOR_ID_CTR_B;
    if (!put(&idb, 4)) return 0;
    return (uint32_t)(p - out);
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

    uint32_t magic = 0, npool = 0;
    get(&magic, 4); get(&npool, 4);
    if (magic != POOL_MAGIC || npool != NPOOL) {
        log_printf("[battle_pools] load: bad header magic=%08x npool=%u\n",
                   magic, npool);
        return;
    }

    for (int i = 0; i < NPOOL; ++i) {
        Pool saved;
        if (!get(&saved, sizeof(Pool))) return;
        uint32_t nblocks = 0;
        if (!get(&nblocks, 4)) return;
        for (uint32_t b = 0; b < nblocks; ++b) {
            uint32_t addr = 0, size = 0;
            if (!get(&addr, 4) || !get(&size, 4)) return;
            if (p + size > end) return;
            // Block addresses are match-stable (pre-grown, never freed) —
            // the saved address is still valid; restore the bytes in place.
            memcpy((void*)(uintptr_t)addr, p, size);
            p += size;
        }
        // Restore the allocator struct itself (free_head, chain, counters).
        *pool_at(i) = saved;
    }

    uint8_t mgr[ACTOR_MGR_BYTES];
    if (!get(mgr, ACTOR_MGR_BYTES)) return;
    memcpy((void*)ACTOR_MGR_ADDR, mgr, ACTOR_MGR_BYTES);
    uint32_t idb = 0;
    if (!get(&idb, 4)) return;
    *(uint32_t*)ACTOR_ID_CTR_B = idb;
}

} // namespace battle_pools
