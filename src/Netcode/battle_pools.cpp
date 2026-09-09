#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "battle_pools.h"
#include "desync_registry.h" // render-only field registry: spans + report annotation
#include "cpp_arena.h"     // trace_alloc — malloc call-site attribution
#include "patch_utils.h"   // _R address literal
#include "util.h"          // thiscall
#include "log.h"

// gekko_bridge publishes the per-advance frame/rb/depth (the boostpool va.x
// probe tags its logs with these).
namespace gekko_bridge { extern int g_trace_frame; extern int g_trace_rb; extern int g_trace_depth; }

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
// b0..b5 reservation timers + input-history ring. These are per-frame
// battle state read every frame by actor scripts, so they MUST roll back
// (they stay in the blob, restored on every load). (Verified via the New
// stubs @0x6D5F0/0x6EA20/0x702A0/0x75480, each of which references its
// std::_Ref_count_obj_alloc<...TPoolAllocator> vtable.)
//
// peer_local: InputSingle/InputMulti/InputCommand are excluded from the
// desync CHECKSUM (but kept in restore, like render_tainted). In DUAL
// netplay each peer instantiates input-device objects for ITS OWN local
// player only (host=slot0, client=slot1), so these pools hold a DIFFERENT
// NUMBER of live objects on each side — a structural, per-peer difference
// that made gekko's frame-0 checksum disagree (cross-peer blob diff: ONLY
// these 3 pools diverged; InputGlobal + every actor/anim/physics pool
// matched byte-for-byte). This is why the SOLO stress rig was clean (one
// process, no cross-peer compare) but dual desynced immediately. Excluding
// them is safe: the actor-facing decoded input (InputGlobal) stays
// checksummed AND matches, and all DOWNSTREAM gameplay state (actors/anim/
// physics/sq) stays checksummed — so any real gameplay divergence still
// aborts, only the per-peer input infrastructure is walked around.
struct PoolRef { uint32_t rva; const char* name; bool render_tainted; bool peer_local; };
static const PoolRef g_pool_rva[] = {
    { 0x49B370, "Actor2DManager/World2D" },
    { 0x49B390, "Actor2DProcGroup" },
    { 0x49B410, "Camera2D", true },
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
    { 0x49B6F0, "Camera3D", true },
    { 0x49B770, "ActorCollisionData" },
    { 0x49B790, "EwActor" },
    { 0x49B450, "InputGlobal" },
    { 0x49B4B0, "InputSingle",  false, true },
    { 0x49B4D0, "InputMulti",   false, true },
    { 0x49B510, "InputCommand", false, true },
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

// TF4::TPoolAllocator::GrowAndAlloc — __thiscall(this=&pool). Allocates a
// block of grow_count slots from the TF4 mesh-vertex mspace, threads them onto
// the free list, stores [prev_block][prev_block_size] in the block's last 8
// bytes, doubles grow_count -- AND THEN POPS AND RETURNS THE BLOCK'S FIRST
// SLOT. It is the allocation slow path, not a plain grow (was guess-named
// Manbow__NetworkNode__BeginStreaming in the IDB; renamed 2026-09-08).
//
// pregrow() therefore has to hand the returned slot back, or every grow leaks
// one permanently-live slot at the base of the block it just made.
typedef uint32_t thiscall grow_and_alloc_t(void* pool);
#define grow_and_alloc ((grow_and_alloc_t*)(0x37C30_R))

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
    // need to grow mid-match (which would change the block set the snapshot
    // walks). 1024 covers a danmaku peak with headroom; grow_count doubles per
    // call so this overshoots to ~2016 slots/pool. save() walks every pregrown
    // slot + its free-list, so this IS a save-cost lever — but reducing it to
    // 256 desynced 0x9999 (nitori/sinmyoumaru) at f=2 (a pool grew past the
    // pregrown set mid-setup -> fwd/resim block-set mismatch). Safe reduction
    // needs per-pool peak-usage data to size each pool individually; until then
    // 1024 stays. SQUIROLL_PREGROW=N overrides for experiments (determinism-risky).
    uint32_t TARGET = 1024;
    { char b[8] = {0};
      if (GetEnvironmentVariableA("SQUIROLL_PREGROW", b, sizeof b) > 0) {
          uint32_t v = 0; for (const char* s = b; *s >= '0' && *s <= '9'; ++s) v = v*10 + (*s-'0');
          if (v >= 32) TARGET = v; } }
    for (int i = 0; i < NPOOL; ++i) {
        Pool* p = pool_at(i);
        if (p->slot_size == 0) continue;  // pool never used yet — leave it
        for (int g = 0; g < 10 && pool_slots(p) < TARGET; ++g) {
            // Push the slot the allocator just handed us straight back onto the
            // free list -- it was never constructed, so this is exactly the
            // pool's own free operation. Dropping it instead left one live slot
            // at the base index of every block: 133 slots of stale junk
            // serialised into every savestate, and (much worse) a permanently
            // live slot at the TOP of each pool's index range, which pinned the
            // snapshot's hot/cold boundary at the end of the pool and made the
            // partition worth 21% instead of 7x.
            uint32_t slot = grow_and_alloc(p);
            if (slot) {
                *(uint32_t*)(uintptr_t)slot = p->free_head;
                p->free_head = slot;
            }
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
    // slot offsets of the collision-box shared_ptr std::vectors to re-home into
    // cpp_arena: 0x88/0x94/0xA0 = base+0x78/0x84/0x90 (col/hit/hurt boxes).
    // NOTE: do NOT add the sprites vector (base+0x224) here — re-homing that live
    // RENDER vector broke HUD rendering (health bar sprites). Its address
    // divergence is handled by a checksum-exclusion span in save() instead.
    static const uint32_t VEC_OFF[3] = { 0x88, 0x94, 0xA0 };
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
static constexpr uint32_t POOL_MAGIC  = 0x4C4F4F50;  // 'POOL' — legacy free-list-as-addresses
static constexpr uint32_t POOL_MAGIC_C = 0x434F4F50; // 'POOC' — canonical free-set bitmap

// Free-slot bitmap scratch, reused per pool — one bit per slot. A pregrown
// pool holds a couple thousand slots (SqFunctionHolder peaks ~4064); 64K bits
// is wide headroom even if a pool grows mid-match.
static constexpr uint32_t MAXSLOT = 65536;
static uint8_t g_freebits[MAXSLOT / 8];
// Second bitmap, used only by SQUIROLL_BPVALIDATE to re-derive the free set
// with a full walk and compare it against the fast partitioned one.
static uint8_t g_freebits_ref[MAXSLOT / 8];

// Checksum-exempt spans of the LAST save(): byte ranges (absolute, into the
// caller's dest buffer) that the desync checksum must skip. Two producers:
//  - whole render-tainted pool records (Camera2D/3D — the renderer writes into
//    those objects), a handful of spans; and
//  - the desync_registry render-only fields of each live slot (matched by
//    object vtable, masked by offset — see the slot loop in save()). Those
//    fields hold pointers into the OS-placed render heap and re-allocate at a
//    different address when their owner is destroyed+recreated inside the
//    rollback window, flagging FALSE desyncs while the gameplay sim matches
//    byte-for-byte (0xEAC9 f2231). Emitted as byte-exact spans because the
//    slot's field grid is only known at save time; a word-mask over the whole
//    variable-length blob cannot align, and value-range classification broke
//    when the heap base moved between sessions.
// Consumed by snapshot_ring::capture()'s masked fold (the ACTIVE checksum) and
// by the fallback trailer2 walk. Spans are emitted in ascending blob order. On
// overflow the excess fields stay checksummed (logged once) — safe: worst case
// is a false desync, never a missed real one.
// --- save/load phase profiler -------------------------------------------
// Four QueryPerformanceCounter calls per save is ~100 ns; the phases they
// separate are hundreds of microseconds each, so this stays on in ship
// config. It is the only way to tell which half of a change paid off:
// medians over 240 saves, because run-to-run variance on this machine is
// larger than most of the wins.
struct BpProf {
    uint64_t walk = 0;    // free-list chase that builds the free bitmap
    uint64_t link = 0;    // canonical relink + free-set emit
    uint64_t slot = 0;    // live-slot copy (+ registry span matching)
    uint64_t lload = 0;   // load: live-slot restore
    uint64_t lfree = 0;   // load: free-list rebuild
    uint32_t nsave = 0, nload = 0;
    uint32_t slots = 0, live = 0, freec = 0, blocks = 0, hot = 0;
};
static BpProf g_prof;
static inline uint64_t qpc() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart;
}
static void log_peaks();
static void prof_report() {
    if (g_prof.nsave < 240) return;
    LARGE_INTEGER fr; QueryPerformanceFrequency(&fr);
    const uint64_t hz = (uint64_t)fr.QuadPart;
    auto us = [&](uint64_t t, uint32_t n) {
        return n ? (uint32_t)(t * 1000000ull / hz / n) : 0u;
    };
    log_printf("[perf-bp] save us: walk=%u link=%u slot=%u | load us: slots=%u "
               "free=%u | slots=%u hot=%u live=%u hotfree=%u blocks=%u\n",
               us(g_prof.walk, g_prof.nsave), us(g_prof.link, g_prof.nsave),
               us(g_prof.slot, g_prof.nsave), us(g_prof.lload, g_prof.nload),
               us(g_prof.lfree, g_prof.nload),
               g_prof.slots, g_prof.hot, g_prof.live, g_prof.freec, g_prof.blocks);
    static int nrep = 0;
    if ((nrep++ % 4) == 0) log_peaks();
    g_prof = BpProf{};
}

// HOT / COLD PARTITION.
//
// pregrow() sizes every pool to ~2016 slots so it never has to grow mid-match
// (a mid-match grow changes the block set between the forward sim and the
// re-sim and desyncs). [bppeak] says what those slots are actually for: 21 of
// the 22 pools peak at 168 live or fewer, and the whole set peaks at ~3,200
// live out of 46,400. So the free-list chase, the canonical relink, the
// bitmap and the load-side rebuild were all being paid on 43,000 slots that
// the match never touches.
//
// `w` is a high-water slot index with the invariant: EVERY slot with index > w
// is free, has never been allocated since the last full walk, and is therefore
// still linked in the ascending canonical chain that the last canonicalisation
// wrote. That makes the cold tail free by construction: the chase stops at it,
// the bitmap ends at w, and the blob carries w instead of the whole pool.
//
// Allocation pops the head and the chain is ascending, so cold slots can only
// ever be consumed in ascending order starting at slot w+1. Breaching the
// boundary is therefore detectable at exactly one place, and THREE independent
// conditions have to hold for the fast path to be taken:
//   1. the chase arrives at slot w+1 (and not at some other cold-index node),
//   2. slot w+1 still carries COLD_MAGIC at offset +4 -- an allocation writes
//      its object over that word and freeing the slot only restores offset 0,
//      so the magic cannot survive a round trip through the allocator, and
//   3. slot w+1 still links to slot w+2.
// Anything else falls back to the full walk, which recomputes w from the
// highest live index. A false breach costs one slow save; a false fast path
// would be a wrong free set, so the checks are deliberately redundant.
//
// `w` is ROLLED-BACK STATE: it goes in the blob and load() restores it, so the
// forward sim and the re-sim (and both peers) always partition identically.
// Without that the two timelines could emit different-length bitmaps for the
// same simulation state and flag a false desync.
struct PoolCache {
    uint32_t total = 0;                  // slot count this cache was built for
    uint32_t w     = 0;                  // high-water: index > w implies free
    uint32_t coldw = 0xFFFFFFFFu;        // boundary the in-memory cold chain holds
    bool     valid = false;
};
static PoolCache g_pc[64];
static constexpr uint32_t COLD_MAGIC = 0xC01DC01Du;

// Per-pool high-water live-slot count. pregrow() sizes EVERY pool to the same
// ~2016 slots, so the walk, the bitmap and the relink are all paid on ~46,400
// slots when the match only ever allocates ~3,000. This is the data needed to
// size each pool individually instead of uniformly; it costs one compare per
// pool per save.
static uint32_t g_peak_live[64];
static uint32_t g_peak_slots[64];
static uint32_t g_maxlive[64];

static void log_peaks() {
    for (int i = 0; i < NPOOL; ++i)
        log_printf("[bppeak] %-22s peak_live=%u slots=%u hot=%u maxlive=%u\n",
                   g_pool_rva[i].name, g_peak_live[i], g_peak_slots[i],
                   g_pc[i].valid ? g_pc[i].w + 1 : 0, g_maxlive[i]);
}

static const int NCS_MAX = 2048;
static const uint8_t* g_ncs_lo[NCS_MAX];
static const uint8_t* g_ncs_hi[NCS_MAX];
static int g_ncs_n = 0;
static bool g_ncs_overflowed = false;
int nochecksum_spans(const uint8_t** lo, const uint8_t** hi, int maxn) {
    int n = (g_ncs_n < maxn) ? g_ncs_n : maxn;
    for (int i = 0; i < n; ++i) { lo[i] = g_ncs_lo[i]; hi[i] = g_ncs_hi[i]; }
    return n;
}
// Append [lo,hi) to the span list, merging with the previous span when adjacent
// (vector begin/end/cap triples produce runs of consecutive dwords).
static void ncs_push(const uint8_t* lo, const uint8_t* hi) {
    if (g_ncs_n > 0 && g_ncs_hi[g_ncs_n - 1] == lo) { g_ncs_hi[g_ncs_n - 1] = hi; return; }
    if (g_ncs_n < NCS_MAX) { g_ncs_lo[g_ncs_n] = lo; g_ncs_hi[g_ncs_n] = hi; ++g_ncs_n; return; }
    if (!g_ncs_overflowed) {
        g_ncs_overflowed = true;
        log_printf("[battle_pools] nochecksum span overflow (>%d) — excess render "
                   "ptrs stay checksummed\n", NCS_MAX);
    }
}

// SQUIROLL_BPCANON=0 restores the pre-2026-09-08 format (free list emitted as
// an address array, in whatever order the allocator left it). Kept only as the
// A/B arm for the measurement; the canonical path is the default.
static bool canon_on() {
    static int v = -1;
    if (v < 0) { char b[8] = {0};
        v = (GetEnvironmentVariableA("SQUIROLL_BPCANON", b, sizeof b) > 0 &&
             b[0] == '0') ? 0 : 1; }
    return v != 0;
}
// SQUIROLL_BPVALIDATE=1 keeps the save/load round-trip self-test running for
// the whole session instead of the first 24 loads, and cross-checks the
// canonical rewrite against a second independent walk. Rule from
// docs/INSTRUMENTATION.md: develop state-serialisation changes against the
// SOLO rig with this on, where a mistake is a log line and not a cross-peer
// desync three minutes into a dual run.
static bool validate_on() {
    static int v = -1;
    if (v < 0) { char b[8] = {0};
        v = (GetEnvironmentVariableA("SQUIROLL_BPVALIDATE", b, sizeof b) > 0 &&
             b[0] != '0') ? 1 : 0; }
    return v != 0;
}

uint32_t save(uint8_t* out, uint32_t cap) {
    uint8_t* p   = out;
    uint8_t* end = out + cap;
    auto put = [&](const void* src, uint32_t n) -> bool {
        if (p + n > end) return false;
        memcpy(p, src, n);
        p += n;
        return true;
    };

    const bool canon = canon_on();
    const bool vald  = validate_on();
    uint32_t magic = canon ? POOL_MAGIC_C : POOL_MAGIC, npool = NPOOL;
    if (!put(&magic, 4) || !put(&npool, 4)) return 0;

    uint32_t st_slots = 0, st_live = 0, st_free = 0, st_blocks = 0, st_hot = 0;
    g_ncs_n = 0;
    for (int i = 0; i < NPOOL; ++i) {
        const uint8_t* pool_rec_start = p;
        Pool* pl = pool_at(i);
        uint32_t ss = pl->slot_size;

        // Enumerate blocks: base address + cumulative slot index + slot count.
        struct Blk { uint32_t addr, size, base_idx, nslots; };
        Blk blk[32];
        uint32_t nblk = 0, total = 0, nblk_seen = 0;
        for_each_block(pl, [&](uint32_t b, uint32_t s) {
            ++nblk_seen;
            if (nblk >= 32) return;
            uint32_t ns = ss ? (s - 8) / ss : 0;
            blk[nblk] = { b, s, 0, ns };
            ++nblk;
        });
        // Slot INDEX runs oldest block first, i.e. BACKWARDS along the chain.
        // TPoolAllocator::Grow links each new block at the head, so pregrow()
        // leaves the empty blocks at the front and the objects that already
        // existed at the back. Indexing in chain order would put those live
        // objects at the highest indices and pin the hot/cold high-water at the
        // end of the pool, which is exactly what it did on the first attempt
        // (`hot` stayed at the full 46,400). Reversed, the pregrown-and-never-
        // touched blocks are the cold tail, which is what they are.
        for (uint32_t b = nblk; b-- > 0; ) { blk[b].base_idx = total; total += blk[b].nslots; }
        if (nblk_seen > 32) {
            // Would silently drop the tail of the pool from BOTH the live set
            // and the free list — the exact shape of a wrong state
            // serialisation. Fail the save instead.
            log_printf("[battle_pools] !! %s has %u blocks (>32) — save aborted\n",
                       g_pool_rva[i].name, nblk_seen);
            return 0;
        }
        if (total > MAXSLOT) return 0;
        st_slots += total; st_blocks += nblk;
        if (i < 64) g_peak_slots[i] = total;

        // Index <-> address inside this pool. Block base_idx is cumulative in
        // chain order, so indices ascend across the block table.
        auto addr_of = [&](uint32_t idx) -> uint32_t {
            for (uint32_t b = 0; b < nblk; ++b)
                if (idx >= blk[b].base_idx && idx < blk[b].base_idx + blk[b].nslots)
                    return blk[b].addr + (idx - blk[b].base_idx) * ss;
            return 0;
        };
        auto index_of = [&](uint32_t fa, uint32_t& out) -> bool {
            for (uint32_t b = 0; b < nblk; ++b)
                if (fa >= blk[b].addr && fa < blk[b].addr + blk[b].nslots * ss) {
                    out = blk[b].base_idx + (fa - blk[b].addr) / ss;
                    return true;
                }
            return false;
        };

        PoolCache& pc = g_pc[i];
        if (!pc.valid || pc.total != total) {   // first save, or the pool grew
            pc.valid = false; pc.total = total; pc.coldw = 0xFFFFFFFFu;
            pc.w = total ? total - 1 : 0;
        }
        uint32_t W = canon ? pc.w : (total ? total - 1 : 0);

        // Walk the free list to build the free bitmap over the HOT range
        // [0, W]. The chase is a dependent load per node and used to cover
        // ~43,000 of them -- the single largest item in the frame. With the
        // partition it covers only the working set; the cold tail is free by
        // construction (see PoolCache).
        const uint64_t t_walk0 = qpc();
        // `retried` = the fast path breached and we fell back. `covered_all` =
        // the walk that succeeded covered every slot (either because the cache
        // was cold, or because of that fallback), which is what licenses
        // recomputing the boundary and rewriting the whole chain.
        bool retried = false, breach = false, outside = false;
        uint32_t mapped = 0;
        if (total) {
            for (int attempt = 0; attempt < 2; ++attempt) {
                const uint32_t cold_first  = (W + 1 < total) ? addr_of(W + 1) : 0;
                const uint32_t cold_second = (W + 2 < total) ? addr_of(W + 2) : 0;
                memset(g_freebits, 0, (W >> 3) + 1);
                mapped = 0; breach = false; outside = false;
                uint32_t fa = pl->free_head, guard = 0;
                while (fa) {
                    if (fa == cold_first) {
                        const uint32_t link = *(const uint32_t*)(uintptr_t)fa;
                        const uint32_t mag  = *(const uint32_t*)(uintptr_t)(fa + 4);
                        if (mag != COLD_MAGIC || link != cold_second) breach = true;
                        break;                       // cold tail intact -> done
                    }
                    if (++guard > total) { breach = true; break; }   // cycle
                    uint32_t idx;
                    if (!index_of(fa, idx)) { outside = true; break; }
                    if (idx > W) { breach = true; break; }           // in the cold tail
                    g_freebits[idx >> 3] |= (uint8_t)(1u << (idx & 7));
                    ++mapped;
                    fa = *(const uint32_t*)(uintptr_t)fa;
                }
                // Chain ended before reaching the cold tail: cold was consumed.
                if (!breach && !outside && fa == 0 && cold_first != 0) breach = true;
                if (outside) break;
                if (!breach) break;
                if (attempt == 1) break;             // the full walk also failed
                W = total - 1; retried = true;       // retry, whole pool hot
            }
        }
        const bool covered_all = (total != 0 && W == total - 1);
        g_prof.walk += qpc() - t_walk0;

        // THE ORACLE for the hot/cold partition. A wrong free set is the worst
        // bug this file can produce -- linking a live object into the free list
        // hands the same memory out twice -- and it does not announce itself:
        // the last attempt at incremental pool tracking was reverted after a
        // run executed pool memory as code during a re-simulation. So under
        // SQUIROLL_BPVALIDATE the set is derived a second time by a full walk
        // and compared. The fast path claims every slot above W is free; the
        // full walk knows. Any disagreement is printed with the slot index.
        if (vald && canon && total && !covered_all && !outside && !breach) {
            memset(g_freebits_ref, 0, (total + 7) / 8);
            uint32_t fa = pl->free_head, guard = 0, refn = 0;
            bool ref_ok = true;
            while (fa && guard <= total) {
                uint32_t idx;
                if (!index_of(fa, idx)) { ref_ok = false; break; }
                g_freebits_ref[idx >> 3] |= (uint8_t)(1u << (idx & 7));
                ++refn; ++guard;
                fa = *(const uint32_t*)(uintptr_t)fa;
            }
            if (!ref_ok) {
                log_printf("[bpvald] !! %s reference walk left the blocks\n",
                           g_pool_rva[i].name);
            } else {
                int bad = 0;
                for (uint32_t idx = 0; idx < total && bad < 4; ++idx) {
                    const bool fast = (idx > W) ||
                        ((g_freebits[idx >> 3] >> (idx & 7)) & 1);
                    const bool ref  = (g_freebits_ref[idx >> 3] >> (idx & 7)) & 1;
                    if (fast != ref) {
                        ++bad;
                        log_printf("[bpvald] !! %s slot %u: partition says %s, "
                                   "full walk says %s (W=%u total=%u)\n",
                                   g_pool_rva[i].name, idx,
                                   fast ? "free" : "live", ref ? "free" : "live",
                                   W, total);
                    }
                }
            }
        }

        if (outside || (breach && retried)) {
            // A node outside every block, or a cycle in the free list. The
            // canonical rewrite would drop or duplicate slots, which changes
            // what the allocator hands out. Refuse rather than quietly repair.
            log_printf("[battle_pools] !! %s free list is malformed (%s) — "
                       "save aborted\n", g_pool_rva[i].name,
                       outside ? "node outside every block" : "cycle");
            return 0;
        }

        // New boundary: the highest LIVE index, plus half again and 64 slots of
        // headroom, so a working set that grows steadily does not breach every
        // frame. Every slot above the walked boundary is free, so the highest
        // live index is always inside the bitmap we just built -- no full walk
        // is needed to recompute this, which is the whole point: the boundary
        // has to be able to come DOWN as well as up. (It could not on the first
        // attempt, so it stayed wherever the very first pre-canonical save put
        // it and the partition only trimmed 21%.)
        const uint32_t Wwalk = W;
        uint32_t newW = W;
        if (total) {
            uint32_t maxlive = 0; bool any = false;
            for (uint32_t idx = Wwalk + 1; idx-- > 0; )
                if (!(g_freebits[idx >> 3] & (1u << (idx & 7)))) {
                    maxlive = idx; any = true; break;
                }
            uint64_t nw = any ? (uint64_t)maxlive + maxlive / 2 + 64 : 64;
            if (nw >= total) nw = total - 1;
            const uint32_t target = (uint32_t)nw;
            if (i < 64) g_maxlive[i] = any ? maxlive : 0;
            // Grow freely; shrink only once the boundary is a quarter too big,
            // because a shrink has to rewrite the cold chain over the whole
            // range it gives back. Hysteresis keeps that off the per-frame path.
            if (covered_all)                    newW = target;
            else if (target < Wwalk - (Wwalk >> 2)) newW = target;
        }
        // The chain has to be rewritten end to end whenever the cold tail moves.
        const bool rewrite_all = covered_all || newW != Wwalk;

        // Canonical rewrite. The free list's ORDER is real state -- it decides
        // which slot the next allocation returns, and a re-simulation has to
        // hand out the same ones -- but it is order we are free to CHOOSE, as
        // long as both peers and both timelines choose identically. Rewriting
        // it into ascending slot-index order at every save makes it a pure
        // function of the free SET, so the blob carries a bitmap over the hot
        // range instead of ~43,000 addresses, the load relinks with one
        // ascending pass instead of a pointer chase, and the next save's walk
        // runs in address order.
        //
        // Ascending SLOT INDEX, not ascending address: the index is
        // (block position in the chain, slot within block), which is identical
        // on both peers by construction, whereas addresses need not be.
        // Writes are elided when the link is already correct -- after the first
        // save most of the list already is.
        const uint64_t t_link0 = qpc();
        uint32_t freec = 0;
        if (canon && total) {
            // After a full walk the whole chain is rewritten (that is what
            // re-establishes the cold tail for the new boundary); otherwise
            // only the hot range, whose last entry links to the cold tail.
            const uint32_t cbound    = rewrite_all ? total - 1 : newW;
            const uint32_t cold_head = (newW + 1 < total) ? addr_of(newW + 1) : 0;
            const uint32_t tail_link = rewrite_all ? 0 : cold_head;
            uint32_t prev = 0, head = 0;
            bool done = false;
            for (uint32_t bi = nblk; bi-- > 0 && !done; ) {
                const uint32_t b = bi;
                const uint32_t a0 = blk[b].addr, n0 = blk[b].nslots;
                uint32_t idx = blk[b].base_idx;
                for (uint32_t k = 0; k < n0; ++k, ++idx) {
                    if (idx > cbound) { done = true; break; }
                    if (idx <= Wwalk && !(g_freebits[idx >> 3] & (1u << (idx & 7))))
                        continue;                       // known live
                    const uint32_t sa = a0 + k * ss;
                    if (prev) {
                        uint32_t* lnk = (uint32_t*)(uintptr_t)prev;
                        if (*lnk != sa) *lnk = sa;
                    } else {
                        head = sa;
                    }
                    prev = sa;
                    if (idx <= newW) ++freec;
                }
            }
            if (prev) {
                uint32_t* lnk = (uint32_t*)(uintptr_t)prev;
                if (*lnk != tail_link) *lnk = tail_link;
            } else {
                head = tail_link;
            }
            pl->free_head = head;
            // Re-stamp the cold sentinel last, so the chain write above cannot
            // clobber it (the chain only ever touches offset 0).
            if (cold_head) *(uint32_t*)(uintptr_t)(cold_head + 4) = COLD_MAGIC;
            pc.valid = true; pc.total = total; pc.w = newW; pc.coldw = newW;
            if (vald && !covered_all && freec != mapped)
                log_printf("[bpvald] !! %s canon hotfree=%u != walked=%u\n",
                           g_pool_rva[i].name, freec, mapped);
        } else {
            freec = mapped;
            newW  = total ? total - 1 : 0;
        }
        g_prof.link += qpc() - t_link0;
        st_free += freec;
        st_hot  += total ? newW + 1 : 0;

        // The allocator struct goes in AFTER the rewrite so free_head matches
        // the emitted bitmap.
        if (!put(pl, sizeof(Pool))) return 0;

        // Block table.
        if (!put(&nblk, 4)) return 0;
        for (uint32_t b = 0; b < nblk; ++b)
            if (!put(&blk[b].addr, 4) || !put(&blk[b].size, 4)) return 0;

        // Every allocated slot, in full. Render-only fields inside the slots
        // (desync_registry: e.g. AnimationController2D font handle + sprites
        // vector backing) diverge across rollback while the sim matches, so each
        // registered field gets a byte-exact nochecksum span (see ncs_push).
        // Matching is by OBJECT VTABLE + field OFFSET — value-independent, so the
        // span sets are identical on both timelines by construction (a value-
        // range scan broke when the OS-placed render heap moved between sessions).
        // Slots are make_shared records: object (and its vtable) at slot+0x10.
        // Render-tainted pools skip this — their whole record is excluded below
        // (inner spans first would also break the span list's ascending order).
        const uint64_t t_slot0 = qpc();
        if (p + 4 > end) return 0;
        uint32_t* nlive = (uint32_t*)p; p += 4;
        uint32_t live = 0;
        const bool try_registry = !g_pool_rva[i].render_tainted &&
                                  !g_pool_rva[i].peer_local && ss >= 0x14;
        bool live_done = false;
        for (uint32_t bi = nblk; bi-- > 0 && !live_done; ) {
            const uint32_t b = bi;
            for (uint32_t j = 0; j < blk[b].nslots; ++j) {
                uint32_t idx = blk[b].base_idx + j;
                if (idx > newW) { live_done = true; break; }   // cold tail: all free
                if (g_freebits[idx >> 3] & (1u << (idx & 7))) continue;  // free
                uint32_t sa = blk[b].addr + j * ss;
                if (!put(&sa, 4) || !put((const void*)(uintptr_t)sa, ss)) return 0;
                if (try_registry) {
                    const uint8_t* content = p - ss;      // slot bytes in the blob
                    if (const auto* t = desync_registry::match_slot(
                            content, ss, (uint32_t)base_address)) {
                        for (int f = 0; f < t->nfields; ++f) {
                            uint32_t lo = t->hdr + t->fields[f].obj_off;
                            uint32_t hi = lo + t->fields[f].len;
                            if (hi <= ss) ncs_push(content + lo, content + hi);
                        }
                    }
                }
                ++live;
            }
        }
        *nlive = live;
        g_prof.slot += qpc() - t_slot0;
        st_live += live;
        if (i < 64 && live > g_peak_live[i]) g_peak_live[i] = live;

        // The free set. Canonical: [total][w][bitmap over the HOT range
        // [0, w]], bit set = free; every slot above w is free by construction
        // and the order is implied. Legacy: [nfree] then one address each.
        if (canon) {
            const uint32_t nbits  = total ? newW + 1 : 0;
            const uint32_t nbytes = (nbits + 7) / 8;
            if (!put(&total, 4) || !put(&newW, 4)) return 0;
            if (nbytes && !put(g_freebits, nbytes)) return 0;
        } else {
            if (p + 4 > end) return 0;
            uint32_t* nfree = (uint32_t*)p; p += 4;
            uint32_t fc = 0;
            for (uint32_t fa = pl->free_head, guard = 0; fa && guard <= total; ++guard) {
                if (!put(&fa, 4)) return 0;
                ++fc;
                fa = *(const uint32_t*)(uintptr_t)fa;
            }
            *nfree = fc;
        }

        // Render-tainted pools: the RENDERER writes into these objects
        // (Camera2D/3D are ConnectRenderSlot'ed — the forward-only draw pass
        // stores matrices/state into them), so their bytes can never match
        // forward-vs-re-sim and would flag false desyncs. They stay IN the
        // blob (restore needs them); the gekko checksum walks around these
        // spans (gekko_bridge save_state_to_buf), exactly like the cpp span.
        // The authoritative sim camera state (::camera table: target/zoom/
        // shake) lives in sq_arena and remains fully checksummed.
        if ((g_pool_rva[i].render_tainted || g_pool_rva[i].peer_local) &&
            g_ncs_n < (int)(sizeof(g_ncs_lo) / sizeof(*g_ncs_lo))) {
            g_ncs_lo[g_ncs_n] = pool_rec_start;
            g_ncs_hi[g_ncs_n] = p;
            ++g_ncs_n;
        }
    }

    if (!put((const void*)ACTOR_MGR_ADDR, ACTOR_MGR_BYTES)) return 0;
    uint32_t idb = *(const uint32_t*)ACTOR_ID_CTR_B;
    if (!put(&idb, 4)) return 0;

    g_prof.slots = st_slots; g_prof.live = st_live;
    g_prof.freec = st_free;  g_prof.blocks = st_blocks;
    g_prof.hot   = st_hot;
    ++g_prof.nsave;
    prof_report();
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

// Diagnostic probe: the player's va.x C++ address (set by the [nuttrace]
// __gekko_watch_va native). boostpool_save/load log the value at this address as
// they process the containing block, so we can see exactly what va.x is SAVED vs
// RESTORED at the f=24 divergence -- and whether the block is in the blob at all.
uint32_t g_va_probe = 0;
void set_va_probe(uint32_t a) { g_va_probe = a; log_printf("[bpprobe] set_va_probe(%08X)\n", a); }
// The frame whose save-blob boostpool_load is currently restoring (set by
// gekko_bridge::load_state from hdr->frame), so the va.x probe can attribute
// the restored value to a specific save.
int g_load_frame = -2;
void set_load_frame(int f) { g_load_frame = f; }

uint32_t boostpool_save(uint8_t* out, uint32_t cap) {
    uint8_t* p = out;
    uint8_t* end = out + cap;
    int _pf = gekko_bridge::g_trace_frame;
    bool _plog = (g_va_probe != 0);
    auto put = [&](const void* s, uint32_t n) -> bool {
        if (p + n > end) return false;
        memcpy(p, s, n);
        p += n;
        return true;
    };

    uint32_t magic = BOOSTPOOL_MAGIC, npool = (uint32_t)NBOOSTPOOL;
    if (!put(&magic, 4) || !put(&npool, 4)) return 0;

    { static int _dn = 0; if (_dn < 4) { _dn++; log_printf("[bpprobe] boostpool_save sees g_va_probe=%08X f=%d\n", g_va_probe, _pf); } }

    // One-time coverage dump: does the player's va.x address fall inside ANY
    // captured boostpool block? If not, va is in a pool absent from
    // g_boostpool_rva (or a block the walk never reaches) -> uncaptured.
    static bool _dumped = false;
    if (g_va_probe && !_dumped) {
        _dumped = true;
        bool covered = false;
        for (int i = 0; i < NBOOSTPOOL; ++i) {
            const Pool* pl = (const Pool*)(g_boostpool_rva[i] + base_address);
            for_each_block(pl, [&](uint32_t b, uint32_t s) {
                bool hit = (g_va_probe >= b && g_va_probe < b + s);
                if (hit) covered = true;
                log_printf("[bpcover] pool#%d rva=%05X blk %08X..%08X%s\n",
                           i, g_boostpool_rva[i], b, b + s, hit ? "  <== VA.X HERE" : "");
            });
        }
        log_printf("[bpcover] va.x @%08X covered_by_boostpool=%d\n", g_va_probe, covered);
    }

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
            if (_plog && g_va_probe >= b && g_va_probe < b + s)
                log_printf("[bpsave] f=%d rb=%d pool#%d SAVE block %08X..%08X "
                           "va.x=%08X (probe in this block)\n", _pf,
                           gekko_bridge::g_trace_rb, i, b, b + s,
                           *(const uint32_t*)(uintptr_t)g_va_probe);
            // [addr][size][size bytes] -- the whole block, including its
            // 8-byte block-list trailer (next-ptr/next-size, stable pointers).
            if (!put(&b, 4) || !put(&s, 4) ||
                !put((const void*)(uintptr_t)b, s)) ok = false;
        });
        if (!ok) return 0;
    }
    return (uint32_t)(p - out);
}

// Committed + writable + in-region sanity for a restore target. This REPLACES the
// old "walk the pool's block list to validate the address" guard, which is
// unreliable inside boostpool_load: this runs BEFORE engine_snap restores the
// pool struct (block_list_head) and before the block trailers are restored, so the
// walk follows the live/un-reverted (and across many rollbacks, cross-linked)
// chain and misses the saved block. A page-state check is the right guard against
// a malformed blob and doesn't depend on the (in-flux) block list.
static bool bp_writable(uint32_t a, uint32_t n) {
    if (!a || !n) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)(uintptr_t)a, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD W = PAGE_READWRITE | PAGE_WRITECOPY |
                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & W)) return false;
    uint32_t rbase = (uint32_t)(uintptr_t)mbi.BaseAddress;
    return (uint64_t)a + n <= (uint64_t)rbase + mbi.RegionSize;
}

// [#28] shared-heap D3D-clobber watchpoint (mirror of engine_snap.cpp). A
// restore write into the D3D-object heap band (~0x05M device/context, ~0x14-
// 0x18M texture objects) clobbers a live D3D COM object = the driver
// type-confusion crash under rollback. Log (first 60) + optionally SKIP the
// write (SQUIROLL_SKIP_D3D_BAND) to prove causation.
static bool bp_d3d_band(uint32_t a) {
    return (a >= 0x14000000u && a < 0x18000000u) ||
           (a >= 0x05000000u && a < 0x06000000u);
}
static bool g_bp_skip_band = []{ const char* e = getenv("SQUIROLL_SKIP_D3D_BAND");
                                 return e && atoi(e); }();
static bool bp_wp(const char* tag, uint32_t a, uint32_t l) {
    if (!bp_d3d_band(a)) return false;
    static int budget = 60;
    if (budget > 0) {
        --budget;
        uint32_t cur = 0; MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void*)(uintptr_t)a, &mbi, sizeof mbi) &&
            mbi.State == MEM_COMMIT) cur = *(const uint32_t*)(uintptr_t)a;
        log_printf("[#28wp] %s restore-write dst=%08x len=%u cur=%08x "
                   "**D3D-HEAP BAND**%s\n", tag, a, l, cur,
                   g_bp_skip_band ? " [SKIPPED]" : "");
    }
    return g_bp_skip_band;
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

    int  _lf = gekko_bridge::g_trace_frame;
    bool _llog = (g_va_probe != 0);
    bool _probe_seen = false;

    for (uint32_t i = 0; i < npool && i < (uint32_t)NBOOSTPOOL; ++i) {
        const Pool* pl = (const Pool*)(g_boostpool_rva[i] + base_address);
        uint32_t nblk = 0;
        if (!get(&nblk, 4)) return;
        for (uint32_t b = 0; b < nblk; ++b) {
            uint32_t addr = 0, size = 0;
            if (!get(&addr, 4) || !get(&size, 4)) return;
            if (p + size > end) return;  // truncated blob guard
            // Restore the saved block DIRECTLY to its saved address. The boostpool
            // TF4 object pools are frozen for the match (tf4_pool freeze-on-grow),
            // so a saved block never moves and is always present at its saved
            // address. We must NOT re-walk the pool's block list to "validate":
            // boostpool_load runs BEFORE engine_snap restores the pool struct
            // (block_list_head) and the block trailers, so the walk follows the
            // live/un-reverted (and across many rollbacks, cross-linked/corrupted)
            // chain and MISSES the saved block -> it got silently skipped -> the
            // player's va/vf/vfBaria were never restored on a rollback -> the f=24
            // first-attack divergence. A committed+writable bound is the only guard
            // needed against a malformed blob.
            // Trust the save: boostpool blocks are frozen for the match
            // (freeze-on-grow) and their addresses were written by THIS battle's
            // boostpool_save from the live pool walk, so a per-block VirtualQuery
            // (bp_writable) on every load was ~5ms of pure overhead. A non-null
            // addr is sufficient; the blob is our own validated data.
            bool ok_target = (addr != 0);
            bool probe_here = (g_va_probe >= addr && g_va_probe < addr + size);
            uint32_t va_live  = probe_here ? *(const uint32_t*)(uintptr_t)g_va_probe : 0;
            uint32_t va_blob  = probe_here ? *(const uint32_t*)(p + (g_va_probe - addr)) : 0;
            if (ok_target && !bp_wp("boost", addr, size))
                memcpy((void*)(uintptr_t)addr, p, size);
            if (probe_here && _llog) {
                _probe_seen = true;
                log_printf("[bpload] loadframe=%d f=%d d=%d pool#%u block %08X ok=%d "
                           "va.x: live=%08X blob=%08X -> now=%08X\n",
                           g_load_frame, _lf, gekko_bridge::g_trace_depth, i, addr, ok_target,
                           va_live, va_blob, *(const uint32_t*)(uintptr_t)g_va_probe);
            }
            p += size;
        }
    }
    if (_llog && !_probe_seen)
        log_printf("[bpload] f=%d d=%d va.x probe @%08X NOT in any saved block\n",
                   _lf, gekko_bridge::g_trace_depth, g_va_probe);
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
    const bool canon = (magic == POOL_MAGIC_C);
    if ((magic != POOL_MAGIC && !canon) || npool != (uint32_t)NPOOL) {
        log_printf("[battle_pools] load: bad header magic=%08x npool=%u\n",
                   magic, npool);
        return;
    }

    for (int i = 0; i < NPOOL; ++i) {
        Pool saved;
        if (!get(&saved, sizeof(Pool))) return;
        uint32_t ss = saved.slot_size;

        // Block table. The canonical format needs it (the free set is by slot
        // index, so index -> address is resolved here); the legacy format
        // restored by absolute address and read past it.
        struct Blk { uint32_t addr, base_idx, nslots; };
        Blk blk[32];
        uint32_t nblk = 0;
        if (!get_u32(nblk)) return;
        if (nblk > 32) return;
        for (uint32_t b = 0; b < nblk; ++b) {
            uint32_t a = 0, sz = 0;
            if (!get_u32(a) || !get_u32(sz)) return;
            blk[b] = { a, 0, ss ? (sz - 8) / ss : 0 };
        }
        // Same reversed index order as save() — oldest block is index 0.
        { uint32_t acc = 0;
          for (uint32_t b = nblk; b-- > 0; ) { blk[b].base_idx = acc; acc += blk[b].nslots; } }

        // Live slots — memcpy each back to its stable address.
        const uint64_t t_l0 = qpc();
        uint32_t nlive = 0;
        if (!get_u32(nlive)) return;
        for (uint32_t k = 0; k < nlive; ++k) {
            uint32_t sa = 0;
            if (!get_u32(sa)) return;
            if (p + ss > end) return;
            if (!bp_wp("bppool", sa, ss)) memcpy((void*)(uintptr_t)sa, p, ss);
            p += ss;
        }
        g_prof.lload += qpc() - t_l0;

        // Free list. Canonical: the saved bitmap is walked in ascending slot
        // index and each free slot's first dword is pointed at the next one —
        // one forward pass with a predictable stride, where the legacy format
        // had to chase the saved address array. Writes are elided when the
        // link already holds the right value, which after a short rollback is
        // almost all of them.
        const uint64_t t_f0 = qpc();
        if (canon) {
            uint32_t total = 0, w = 0;
            if (!get_u32(total) || !get_u32(w)) return;
            const uint32_t nbits  = total ? w + 1 : 0;
            const uint32_t nbytes = (nbits + 7) / 8;
            if (p + nbytes > end) return;
            const uint8_t* bits = p; p += nbytes;
            // Rebuild the hot chain [0, w] in ascending order and hand off to
            // the cold tail. The cold tail is rewritten only when the boundary
            // it currently holds is not the one being restored -- that happens
            // on the first restore, and whenever a rollback crosses a save
            // where the working set grew. w is part of the blob precisely so
            // both timelines partition the same way (see PoolCache).
            PoolCache& pc = g_pc[i];
            const bool need_cold = !pc.valid || pc.total != total || pc.coldw != w;
            const uint32_t cbound = need_cold ? (total ? total - 1 : 0) : w;
            auto addr_of = [&](uint32_t x) -> uint32_t {
                for (uint32_t b = 0; b < nblk; ++b)
                    if (x >= blk[b].base_idx && x < blk[b].base_idx + blk[b].nslots)
                        return blk[b].addr + (x - blk[b].base_idx) * ss;
                return 0;
            };
            const uint32_t cold_head = (w + 1 < total) ? addr_of(w + 1) : 0;
            const uint32_t tail_link = need_cold ? 0 : cold_head;
            uint32_t prev = 0, idx = 0;
            bool done = false;
            for (uint32_t bi = nblk; bi-- > 0 && !done; ) {
                const uint32_t a0 = blk[bi].addr, n0 = blk[bi].nslots;
                for (uint32_t k = 0; k < n0; ++k, ++idx) {
                    if (idx > cbound) { done = true; break; }
                    // Above w every slot is free by construction.
                    if (idx <= w && !(bits[idx >> 3] & (1u << (idx & 7)))) continue;
                    const uint32_t sa = a0 + k * ss;
                    if (prev) {
                        uint32_t* lnk = (uint32_t*)(uintptr_t)prev;
                        if (*lnk != sa) *lnk = sa;
                    }
                    prev = sa;
                }
            }
            if (prev) {
                uint32_t* lnk = (uint32_t*)(uintptr_t)prev;
                if (*lnk != tail_link) *lnk = tail_link;
            }
            if (cold_head) *(uint32_t*)(uintptr_t)(cold_head + 4) = COLD_MAGIC;
            pc.valid = true; pc.total = total; pc.w = w; pc.coldw = w;
        } else {
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
        }
        g_prof.lfree += qpc() - t_f0;

        // Restore the allocator struct (free_head, chain, counters) last.
        *pool_at(i) = saved;
    }

    uint8_t mgr[ACTOR_MGR_BYTES];
    if (!get(mgr, ACTOR_MGR_BYTES)) return;
    memcpy((void*)ACTOR_MGR_ADDR, mgr, ACTOR_MGR_BYTES);
    uint32_t idb = 0;
    if (!get_u32(idb)) return;
    *(uint32_t*)ACTOR_ID_CTR_B = idb;
    ++g_prof.nload;

    // Re-serialise from the just-restored pools and compare to the blob — a
    // mismatch means save/load is not a faithful round-trip. This is THE
    // oracle for any change to this file: it catches a wrong free set or a
    // wrong live set here, as a log line, instead of as a cross-peer desync or
    // an `eip` inside the pool region minutes later. Capped at 24 loads by
    // default (the blob is ~0.25 MB, so it is cheap but not free);
    // SQUIROLL_BPVALIDATE=1 runs it for the whole session.
    static int bp_selftest = 24;
    if (bp_selftest > 0 || validate_on()) {
        if (bp_selftest > 0) --bp_selftest;
        static constexpr uint32_t RE_CAP = 24u * 1024 * 1024;
        static uint8_t* re = nullptr;
        if (!re) re = (uint8_t*)VirtualAlloc(nullptr, RE_CAP,
                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (re) {
            // The self-test's own save() must not be counted in the profile.
            const BpProf keep = g_prof;
            uint32_t rn = save(re, RE_CAP);
            g_prof = keep;
            if (rn != len || memcmp(re, blob, len) != 0) {
                uint32_t d = 0, m = rn < len ? rn : len;
                while (d < m && re[d] == blob[d]) ++d;
                log_printf("[bp] ROUND-TRIP FAIL len=%u vs %u  first-diff@%u\n",
                           len, rn, d);
            } else if (!validate_on()) {
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
// Free-section size in a save() blob. Canonical ('POOC'): [total][bitmap],
// one bit per slot. Legacy ('POOL'): [nfree][nfree x addr].
static inline uint32_t free_sect_bytes(const uint8_t* b, uint32_t off, bool canon) {
    if (!canon) return 4 + *(const uint32_t*)(b + off) * 4;   // [nfree][addrs]
    const uint32_t total = *(const uint32_t*)(b + off);       // [total][w][bits]
    const uint32_t w     = *(const uint32_t*)(b + off + 4);
    return 8 + ((total ? w + 1 : 0) + 7) / 8;
}
// Walk the save() blob (fwd's structure) and report EVERY diverging live slot
// (pool / slot real address / first diverging field + value), capped. Reporting
// all -- not just the first -- shows whether the actor (Actor2D, late in the
// blob) is the root or whether AnimCtrl2D (early in the blob) diverges on its
// own. Requires matching structure (same liveness); a length mismatch is noted
// by the caller and the walk is best-effort.
void bplive_decode(const uint8_t* fwd, const uint8_t* re, uint32_t len) {
    const bool canon = (*(const uint32_t*)fwd == POOL_MAGIC_C);
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
        off += free_sect_bytes(fwd, off, canon);
    }
}
}  // namespace

// DESYNC REPORT (the GDC-style "DesyncUtil", built in): walk a forward/re-sim
// bp-section pair and print EVERY diverging live slot with everything named —
// pool, slot address, object vtable -> registry type, field offset -> registry
// field — and a verdict per dword: KNOWN-RENDER (excluded from the checksum,
// expected to differ) vs UNKNOWN (new divergence: the thing to triage). Called
// automatically from the desync-abort path on the stashed pair, so a single
// run's log contains the complete analysis (no offline cmp/xxd/IDA loop).
void diff_report(const uint8_t* fwd, const uint8_t* re, uint32_t len) {
    if (!fwd || !re || len < 8) { log_printf("[bpreport] no stash pair\n"); return; }
    log_printf("[bpreport] ==== bp divergence report (%u bytes) ====\n", len);
    const bool canon = (*(const uint32_t*)fwd == POOL_MAGIC_C);
    uint32_t off = 8;  // skip [magic][npool]
    int slots_reported = 0, unknown_dwords = 0, known_dwords = 0;
    for (int i = 0; i < NPOOL; ++i) {
        if (off + sizeof(Pool) + 4 > len) break;
        const Pool* pl = (const Pool*)(fwd + off);
        uint32_t ss = pl->slot_size ? pl->slot_size : 1;
        const char* nm = g_pool_rva[i].name;
        off += sizeof(Pool);
        uint32_t nblk = *(const uint32_t*)(fwd + off); off += 4 + nblk * 8;
        if (off + 4 > len) break;
        uint32_t nlive = *(const uint32_t*)(fwd + off); off += 4;
        for (uint32_t s = 0; s < nlive; ++s) {
            if (off + 4 > len) goto done;
            uint32_t sa = *(const uint32_t*)(fwd + off);
            uint32_t c0 = off + 4;                     // slot content start
            if (c0 + ss > len) goto done;
            off = c0 + ss;
            bool div = memcmp(fwd + c0, re + c0, ss) != 0;
            if (!div) continue;
            // Identify the slot's object type (make_shared: object at +hdr,
            // per-type — Actor2D +0xC, AnimCtrl2D +0x10).
            const desync_registry::SlotType* t =
                desync_registry::match_slot(fwd + c0, ss, (uint32_t)base_address);
            if (slots_reported < 24) {
                ++slots_reported;
                if (t) {
                    log_printf("[bpreport] pool='%s' slot=%08X type=%s\n",
                               nm, sa, t->type_name);
                } else {
                    // unknown: print both plausible object-vtable probes so the
                    // registry extension is a single IDA lookup.
                    uint32_t vc = 0, v10 = 0;
                    if (ss >= 0x10) memcpy(&vc,  fwd + c0 + 0x0C, 4);
                    if (ss >= 0x14) memcpy(&v10, fwd + c0 + 0x10, 4);
                    log_printf("[bpreport] pool='%s' slot=%08X type=UNKNOWN "
                               "(vt? +0xC rva %05X / +0x10 rva %05X)\n",
                               nm, sa, (uint32_t)(vc - base_address),
                               (uint32_t)(v10 - base_address));
                }
                for (uint32_t k = 0; k + 4 <= ss; k += 4) {
                    uint32_t fv, rv;
                    memcpy(&fv, fwd + c0 + k, 4);
                    memcpy(&rv, re  + c0 + k, 4);
                    if (fv == rv) continue;
                    const desync_registry::Field* fld =
                        (t && k >= t->hdr)
                            ? desync_registry::find_field(t, k - t->hdr)
                            : nullptr;
                    if (fld) ++known_dwords; else ++unknown_dwords;
                    log_printf("[bpreport]   +0x%03X (obj+0x%03X) fwd=%08X "
                               "resim=%08X  %s%s\n",
                               k, (t && k >= t->hdr) ? k - t->hdr : k, fv, rv,
                               fld ? "KNOWN-RENDER: " : "** UNKNOWN **",
                               fld ? fld->name : "");
                }
            } else {
                // past the print cap: still tally verdicts for the summary
                for (uint32_t k = 0; k + 4 <= ss; k += 4) {
                    uint32_t fv, rv;
                    memcpy(&fv, fwd + c0 + k, 4);
                    memcpy(&rv, re  + c0 + k, 4);
                    if (fv == rv) continue;
                    const desync_registry::Field* fld =
                        (t && k >= t->hdr)
                            ? desync_registry::find_field(t, k - t->hdr)
                            : nullptr;
                    if (fld) ++known_dwords; else ++unknown_dwords;
                }
            }
        }
        if (off + 4 > len) break;
        const uint32_t hdr   = canon ? 8u : 4u;   // [total][w] vs [nfree]
        const uint32_t nfree = *(const uint32_t*)(fwd + off);
        const uint32_t fl0   = off + hdr;
        const uint32_t flen  = free_sect_bytes(fwd, off, canon) - hdr;
        off = fl0 + flen;
        // Free-SET divergence (canonical) or allocation-ORDER divergence
        // (legacy) — either way a real sim signal, not render noise.
        if (off <= len && memcmp(fwd + fl0, re + fl0, flen) != 0) {
            ++unknown_dwords;
            log_printf("[bpreport] pool='%s' FREE-%s diverges (%u slots) — "
                       "allocation nondeterminism ** UNKNOWN **\n",
                       nm, canon ? "SET" : "LIST", nfree);
        }
    }
done:
    log_printf("[bpreport] ==== verdict: %d KNOWN-RENDER dword(s), %d UNKNOWN "
               "dword(s)%s ====\n", known_dwords, unknown_dwords,
               unknown_dwords == 0 ? " — divergence fully explained by the "
               "registry (checksum should NOT have fired; check span plumbing)"
               : "");
}

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
