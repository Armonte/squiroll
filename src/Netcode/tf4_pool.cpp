// safetyhook MUST be included before any squiroll header — util.h #defines
// the calling-convention keywords as attribute macros and safetyhook uses
// those identifiers as method names. (Same ordering constraint as cpp_arena.cpp.)
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>

#include "tf4_pool.h"
#include "cpp_arena.h"
#include "patch_utils.h"   // _R address literal, hotpatch_rel32
#include "util.h"          // thiscall
#include "log.h"

namespace tf4_pool {
namespace {

// SQVM__Call_0 (th155 0x45D20) grows the Squirrel-instance object pool. At
// 0x45D99 it does `call TF4__MeshVertex__PoolAlloc` (E8 rel32) with the
// mspace in ecx and the slab size in edx (__fastcall). Redirecting that one
// call site moves every pool slab into cpp_arena without disturbing the
// mspace. hotpatch_rel32 takes the address of the rel32 OPERAND — one byte
// past the E8 opcode at 0x45D99.
//
// HISTORICAL NOTE: an inline-hook on TF4__MeshVertex__PoolAlloc's entry
// (catching every caller through one gate) was tested — it caught
// Manbow::NetworkNode::BeginStreaming's 342KB streaming buffer too, whose
// receive content is inherently non-deterministic, and produced an
// EARLIER cpp_arena divergence (f=10). The right discriminator isn't
// "sim thread" but "battle pool vs network/audio/etc" — narrow at the
// known battle call site for now; a per-caller exclusion list at the
// entry would be the next step if more battle pool grow sites surface.
#define POOL_ALLOC_REL32  (0x45D9A_R)

// The original TF4 mspace allocator we replaced. Used as the fallback for
// non-simulation-thread grows: the SAME generic tf4_objpool_grow services
// audio (Ogg memory-stream reader) pools too — sending those slabs into
// cpp_arena leaks the audio thread's non-deterministic position updates
// into the rollback snapshot (proven by the DR0 multi-thread watchpoint).
typedef void* (__fastcall *tf4_mspace_pool_alloc_t)(void* mspace, uint32_t size);
#define TF4_MESHVERTEX_POOLALLOC \
    ((tf4_mspace_pool_alloc_t)(uintptr_t)(0x356A0_R))

// __fastcall: arg1 in ecx (mspace), arg2 in edx (slab size in bytes).
// Simulation thread -> cpp_arena (slab is rollback-snapshotted, needed for
// battle pools). Any other thread -> fall through to the original mspace
// allocator so audio / loader / etc. slabs stay outside the snapshot.
static void* __fastcall pool_alloc_redirect(void* mspace, uint32_t size) {
    if (cpp_arena::is_sim_thread()) {
        // Pass our return address — the th155 pool-grow call site (inside
        // tf4_objpool_grow) — so the arena tags the slab with real th155
        // code rather than this shim (keeps cpp_arena::attribute meaningful).
        return cpp_arena::raw_alloc(size, (uint32_t)(uintptr_t)_ReturnAddress());
    }
    return TF4_MESHVERTEX_POOLALLOC(mspace, size);
}

// === objpool pre-grow (freeze the generic-grow pool family) ================
// tf4_objpool_grow (0x45D20) is th155's GENERIC fixed-size object-pool grower
// — 36 call sites, one control struct each (the Squirrel-instance pool, the
// effect-actor pool, mesh vertex/node pools, ...). Its control struct is the
// same 0x1C TF4::TPoolAllocator layout battle_pools uses, BUT it is a
// different grow routine, so battle_pools::pregrow (which only drives the
// 0x37C30 family at 0x49B3xx) never freezes it. Result: these pools GROW
// on-demand DURING a match — i.e. inside the rollback window — and a grow
// does a sorted-by-address free-list insertion walk. Once the upstream sim
// has diverged, that walk follows a divergent/half-restored link and spins
// forever at 0x45E90 (the f=34 hang). Freezing the pools at arm (so they
// never grow mid-match, exactly like battle_pools does for its family) keeps
// the block set stable for the snapshot and removes the in-window grow.
//
// The control structs are passed by PARAMETER at the 36 sites (not loadable
// as static globals), so we auto-discover them: hook the grow, and record
// every pool that grows on the SIM thread (those slabs live in cpp_arena;
// non-sim/audio pools draw from the mspace and must be left alone).
struct ObjPool {                 // 0x1C TF4::TPoolAllocator
    uint32_t free_head;          // +0x00
    uint32_t block_head;         // +0x04
    uint32_t last_block_size;    // +0x08
    uint32_t slot_size;          // +0x0C  (0 = pool never used)
    uint32_t grow_count;         // +0x10  next-grow slot count (doubles)
    uint32_t grow_count_init;    // +0x14
    uint32_t max_objects;        // +0x18  0 = uncapped
};

#define OBJPOOL_GROW  (0x45D20_R)

// grow_count (control+0x10) is the next-grow slot count and doubles each grow;
// freezing a pool until grow_count reaches this leaves it with ~2*TARGET total
// slots — battle_pools' danmaku-peak headroom. A pool this big never grows
// again mid-match, so it never runs the corrupting sorted-insert walk inside
// the rollback window (the f=34 hang at 0x45E90).
static constexpr uint32_t FREEZE_TARGET = 1024;

static SafetyHookInline g_h_objgrow{};
static void*            g_objpools[256];
static int              g_n_objpools = 0;
static CRITICAL_SECTION g_objpool_lock;

static void record_objpool(void* ctrl) {
    EnterCriticalSection(&g_objpool_lock);
    bool found = false;
    for (int i = 0; i < g_n_objpools; ++i)
        if (g_objpools[i] == ctrl) { found = true; break; }
    if (!found && g_n_objpools < 256) {
        g_objpools[g_n_objpools++] = ctrl;
        log_printf("[tf4_pool] objpool discovered #%d ctrl=%08X slot=%u\n",
                   g_n_objpools, (uint32_t)(uintptr_t)ctrl,
                   ((ObjPool*)ctrl)->slot_size);
    }
    LeaveCriticalSection(&g_objpool_lock);
}

// Grow `pool` until grow_count >= FREEZE_TARGET. tf4_objpool_grow also POPS a
// slot each call (returns it), so push that slot back onto the free list to
// keep the pre-grow pure-capacity (no leaked slots). The free-list link is at
// slot+0 and pop is LIFO from free_head, so head push/pop is exact.
static void freeze_pool(ObjPool* p) {
    for (int g = 0; g < 18 && p->grow_count < FREEZE_TARGET; ++g) {
        uint32_t prev = p->grow_count;
        uint32_t slot = (uint32_t)(uintptr_t)g_h_objgrow.unsafe_thiscall<int>((int)(uintptr_t)p);
        if (slot) {                       // return the popped slot to the list
            *(uint32_t*)(uintptr_t)slot = p->free_head;
            p->free_head = slot;
        }
        if (p->grow_count <= prev) break; // capped at max_objects
    }
}

// tf4_objpool_grow (0x45D20) — __thiscall(this=&pool). Only sim-thread (battle)
// pools grow into cpp_arena (loader/audio pools draw from the mspace and must
// be left alone). FREEZE-ON-GROW: the first time any battle pool grows — at
// ANY frame, including a first hit AFTER arm — immediately grow it big so it
// never grows again. The decision keys on the pool's own grow_count, which is
// rolled-back state (in .data, captured by engine_snap), so the freeze fires
// identically on the forward pass and every re-sim -> deterministic.
static int thiscall objpool_grow_hook(int pool) {
    int r = g_h_objgrow.unsafe_thiscall<int>(pool);   // honor the requested grow
    if (cpp_arena::is_sim_thread()) {
        record_objpool((void*)(uintptr_t)pool);
        freeze_pool((ObjPool*)(uintptr_t)pool);
    }
    return r;
}

} // namespace

void install() {
    InitializeCriticalSection(&g_objpool_lock);
    hotpatch_rel32(POOL_ALLOC_REL32, pool_alloc_redirect);
    g_h_objgrow = safetyhook::create_inline((void*)OBJPOOL_GROW,
                                            (void*)objpool_grow_hook);
    log_printf("[tf4_pool] Squirrel-instance pool slabs -> cpp_arena; "
               "objpool grow-hook %s\n", g_h_objgrow.enabled() ? "OK" : "FAIL");
}

// Freeze every recorded battle objpool: grow it now (at arm, on the sim
// thread, so the fresh slabs land in cpp_arena and ride the snapshot) until
// it holds enough slots that a danmaku-heavy match never grows it again. A
// pool that never grows mid-match never runs the corrupting sorted-insert
// walk inside the rollback window. Idempotent (grow_count check), so safe to
// call once per session arm. Mirrors battle_pools::pregrow for the other
// allocator family.
void pregrow_objpools() {
    if (!g_h_objgrow.enabled()) {
        log_printf("[tf4_pool] pregrow_objpools: grow-hook not installed\n");
        return;
    }
    // Freeze every pool discovered so far. Pools first used AFTER arm are
    // frozen on their first grow by the hook (freeze_pool); this catches the
    // ones already used by arm so they're frozen at a clean pre-re-sim point.
    EnterCriticalSection(&g_objpool_lock);
    int n = g_n_objpools;
    void* recs[256];
    for (int i = 0; i < n; ++i) recs[i] = g_objpools[i];
    LeaveCriticalSection(&g_objpool_lock);

    int pools = 0;
    for (int i = 0; i < n; ++i) {
        ObjPool* p = (ObjPool*)recs[i];
        if (!p->slot_size) continue;          // never actually used
        ++pools;
        freeze_pool(p);                       // slab -> cpp_arena (sim thread at arm)
    }
    log_printf("[tf4_pool] pregrow_objpools: %d/%d battle pools frozen at arm "
               "(rest freeze on first grow)\n", pools, n);
}

} // namespace tf4_pool
