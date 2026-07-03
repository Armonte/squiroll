#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "engine_snap.h"
#include "patch_utils.h"   // _R address literal
#include "log.h"

namespace engine_snap {
namespace {

// Scheduler object pointer slots (verified in IDA — also used live by
// gekko_bridge's advance_one_frame, so these are known-good).
#define G_SCRIPTAPI_PTR  ((void**)(0x4DACFC_R))  // Act::ScriptAPI*
#define G_FRAMEDRV_PTR   ((void**)(0x49B01C_R))  // RunOneFrame driver*
#define G_FRAME_COUNTER  ((void*)(0x4DACE0_R))   // ++ per logical frame
#define G_ACTOR_TASK_ID  ((void*)(0x4DB068_R))   // Actor2D SetTask* id ctr
// Manbow::Actor2DManager::LinkActor (0x9AEB0) assigns each newly-linked actor a
// monotonic serial from this global (actor[+232] = (*0x4DCEE8)++). It was NOT
// snapshotted, so after a rollback the counter never rewinds: the re-sim of a
// frame hands its actors DIFFERENT serial ids than the forward pass did, which
// diverges actor[+232] (cpp_arena) and cascades into the boost::signals2
// connection-list pointers (the f=8 onset). Same class as G_ACTOR_TASK_ID.
#define G_ACTOR_SERIAL_ID ((void*)(0x4DCEE8_R))  // Actor2DManager::LinkActor serial

// Manbow effect-system singletons (verified 2026-05-24 via the IDB rename
// pass). Both objects are heap-allocated at startup (operator new) and
// sit OUTSIDE every tracked arena, so the live pointers persist across
// a snapshot-load while the objects they point to (e.g. cEftGroup* in
// cpp_arena) get restored — leaving stale pointers in the singletons'
// vectors. The eft_freer_log harness cleans that symptom up; this is
// the structural fix.
//
//   Ew::sEffect  @ *G_EW_SEFFECT  — 0x218 bytes. Holds the live-groups
//                                    vector at +0xEC/+0xF0/+0xF4.
//   Ew::sTask    @ *G_EW_STASK    — 0x1AAC8 bytes total, but we only
//                                    need the 32 per-layer vector
//                                    triples at +0x18024..+0x181A4 and
//                                    a handful of status/counter slots.
#define G_EW_SEFFECT     ((void**)(0x4DB0C8_R))
#define G_EW_STASK       ((void**)(0x4DB0B8_R))
static constexpr uint32_t SEFFECT_BYTES = 0x218;
// sTask layout (verified):
//   +0x18014  worker-frame head counter (Interlocked, incremented by
//             DispatchWorkerTask + reset at start of StepLayers32)
//   +0x18024..+0x181A4   32 × {begin,end,cap} layer-task-array triples
//   +0x18228..+0x1AAC0   per-layer state structs (timing, smoothing)
//                        — captured by cpp_arena's dirty-page tracking;
//                        only the triples NEED restoration to refer to
//                        the right buffers.
//   +0x1AAC0  current frame time (float)
//   +0x1AAC4  byte flag: timed-update enabled
//   +0x1AAC7  byte flag: step-in-progress
#define STASK_LAYER_BASE_OFF  0x18024
#define STASK_LAYER_COUNT     32
#define STASK_LAYER_TRIPLE    0x0C
#define STASK_FLAGS_OFF       0x1AAC0
#define STASK_FLAGS_BYTES     0x08    // covers float-time + 4 status bytes
#define STASK_FRAME_OFF       0x18014
#define STASK_FRAME_BYTES     0x10    // 4 dwords of counters around 0x18014

// th155 CRT __acrt_getptd() — returns the calling thread's per-thread data
// block. The CRT rand() seed (_holdrand) lives at offset 0x18 within it.
// Squirrel's global rand()/srand() (the battle PRNG) wrap this CRT rand;
// ~150 actor scripts pick animation takes via rand()%N, so the seed MUST be
// part of the rollback snapshot or a re-sim desyncs (wrong takes, wrong AI
// branches). save/load run on the battle thread — the same thread the
// scripts' rand() runs on — so getptd() resolves the same ptd here.
typedef uintptr_t (*acrt_getptd_t)();
#define TH155_ACRT_GETPTD ((acrt_getptd_t)(0x319663_R))
static constexpr uint32_t CRT_HOLDRAND_OFF = 0x18;

// byte_4DAF00 — th155's DirectInput keyboard state table, 256 key bytes
// (high bit = key down; sub_3B850 fills it via IDirectInputDevice8::
// GetDeviceState). The battle's input decode (sub_1687D0) reads it every
// frame to build per-button held-frame counters in the InputSingle pool
// objects. It is polled from the LIVE keyboard once per real frame, so a
// rollback re-sims a logical frame across several real frames — without
// capture, the re-sim reads whatever the keyboard is NOW, and a physical
// keypress landing between a forward frame and its re-sim desyncs the
// input decode (verified: held-counter divergence at InputSingle obj+0x10).
#define KBD_STATE_ADDR  ((void*)(0x4DAF00_R))
static constexpr uint32_t KBD_STATE_BYTES = 256;

// Act::ScriptAPI object size — operator new(0x108) at init.
static constexpr uint32_t SCRIPTAPI_BYTES = 0x108;
// Its four std::list members: _Myhead (sentinel ptr) lives at these
// object offsets, _Mysize at +4 (inside the object, captured already).
static const uint32_t SAPI_LIST_OFF[4] = { 0x64, 0x6C, 0xA8, 0xB0 };
// RunOneFrame driver object size.
static constexpr uint32_t FRAMEDRV_BYTES = 0x14;
// MSVC _List_node sentinel: only _Next/_Prev are live (the sentinel has
// no value) — 8 bytes is sufficient and stays well inside any node.
static constexpr uint32_t SENTINEL_BYTES = 8;

static constexpr uint32_t SNAP_MAGIC = 0x50414E53;  // 'SNAP'
// Bumped from 16 → 24 to fit the new Ew::sEffect / Ew::sTask regions
// (sEffect: 1 region; sTask: 3 regions — counters, triples, status).
static constexpr int      MAX_REGIONS = 64;

// True if [p, p+len) lies entirely in committed, accessible memory.
// Resolving the scheduler graph walks reverse-engineered struct offsets;
// validating every region turns a wrong offset into a skipped region
// (-> a detectable desync) instead of a crash.
// True if EVERY page of [p, p+len) is committed + readable. Walks the range
// rather than requiring a single VirtualQuery region, so a span that crosses a
// PAGE_WRITECOPY->PAGE_READWRITE boundary (a .data page becomes RW on first
// write) still passes. That protection split is non-deterministic across a
// forward advance vs its rollback re-sim, so requiring one region made the
// .data capture split — and thus the blob structure and its checksum —
// diverge even when the bytes matched (the residual `eng` desync). Reads and
// writes are both fine on WRITECOPY pages (a write just COWs to a private RW).
static bool region_ok(const void* p, uint32_t len) {
    if (!p || len == 0) return false;
    uintptr_t a = (uintptr_t)p, e = a + len;
    while (a < e) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void*)a, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        uintptr_t rend = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (rend <= a) return false;
        a = rend;
    }
    return true;
}

struct Region { void* addr; uint32_t len; };

// Resolve the fixed regions that make up the scheduler snapshot. Done
// fresh each call; every address is stable for the whole battle, so a
// region recorded at save time is still valid at the matching load.
static int collect(Region* r) {
    int n = 0;
    auto add = [&](void* a, uint32_t l) {
        if (n < MAX_REGIONS && region_ok(a, l)) {
            r[n].addr = a;
            r[n].len  = l;
            ++n;
        } else {
            // A silently-dropped region is a snapshot GAP (this is exactly how
            // the whole-.data copy was lost). Log them once so other gaps like
            // .data surface instead of hiding as residual divergence.
            static int faillog = 0;
            if (faillog < 40) {
                ++faillog;
                log_printf("[engine_snap] DROPPED region addr=%p len=0x%X (region_ok fail%s)\n",
                           a, l, (n >= MAX_REGIONS) ? " / table full" : "");
            }
        }
    };

    // th155.exe .data section in full — RVA 0x498000, virtual size 0x47AA4
    // (~293 KB), the whole writable static-data segment. This is one region
    // that captures EVERY engine global at once: the scheduler counters,
    // the DirectInput keyboard table (byte_4DAF00), every Squirrel/engine
    // global pointer slot, and — the reason this is a whole-section copy
    // rather than hand-picked fields — every custom pool / free-list
    // allocator th155 keeps in static storage. th155 has many: e.g. the
    // Squirrel-instance object pool whose free-list head lives at 0x4DCD00
    // (sub_45710 pops it; sub_45D20 grows it). A pool head mutates on every
    // alloc/free, so missing even one desyncs a rollback re-sim and then
    // dereferences a stale node -> crash. Chasing pools one at a time is a
    // losing game; at ~293 KB the full-section copy is trivially cheap and
    // strictly supersedes the hand-picked engine globals below (those are
    // kept only as a backstop / for the few that are NOT in .data).
    // Added as committed SUB-REGIONS, not one add(): a single add() of the
    // whole [0x498000, +0x47AA4) range FAILS region_ok whenever the loader
    // splits .data by page protection (it does) — region_ok requires the whole
    // range inside ONE VirtualQuery region, so the ENTIRE engine-global
    // snapshot was silently dropped. That is why hand-picked counters (frame,
    // task id, actor serial 0x4DCEE8) had to be re-added one at a time and
    // STILL left systematic residual divergence (other un-captured globals).
    // Iterating the sub-regions snapshots the whole section in one shot.
    {
        uintptr_t dstart = (uintptr_t)(0x498000_R);
        uintptr_t dend   = dstart + 0x47AA4;
        // Excluded sub-ranges: LIVE / non-deterministic .data that must stay OUT
        // of the snapshot+checksum because it is re-polled or free-running every
        // frame and is NOT simulation state (the sim's real input comes from the
        // GekkoNet replay, not these raw buffers). Sorted ascending, disjoint:
        //   [0x4DAEB8,+20) DirectInput GetDeviceState device buffer
        //                  (dword_4D9F0C[1003]) — _input_get_states polls the live
        //                  joystick/device into it each frame; the forward read
        //                  varies while the headless re-sim's doesn't → false
        //                  desync (pinned via [engdiff]: the residual intermittent
        //                  eng divergence at 0x4DAEB8/0x4DAEBC).
        //   [0x4DB004,+4)  g_engine_loop_tick — free-running main-loop counter
        //                  (++ per Manbow_main_game_loop, read only by
        //                  SoundPlayer::Play), diverges ±1 forward-vs-re-sim.
        //   [0x4DAD10,+4)  _Wndproc window/input state (async OS message handler)
        //                  — non-deterministic across the headless re-sim.
        //   [0x4DB310,+0xA8) background-task condvar/mutex block — cond 0x4DB310,
        //                  g_concurrency_task_cv_mutex 0x4DB338, queued-task slot
        //                  0x4DB340 (a heap closure ptr, 0 when empty), mutex C
        //                  0x4DB398. Written by producer/consumer threads at
        //                  WALL-CLOCK time: fwd frame N had slot=0, the re-sim of
        //                  N sees a live pointer → eng checksum mismatch = the
        //                  f=716 DESYNC ([engdiff] rva 4DB008+0x338). Thread
        //                  plumbing, not sim state.
        //   [0x4DB6A8,+16) bg-ScriptAPI FIFO storage (closure ptr 0x4DB6AC,
        //                  count 0x4DB6B0) — same wall-clock class; a rolled-back
        //                  half-consumed slot is also the null-boost::function
        //                  the re-sim kept invoking (clguard rva 0x32425 skips).
        // NOTE: add_data (below) carves these out of the .data run assuming they
        // are ASCENDING by .lo — it walks the run once, advancing a cursor. An
        // out-of-order entry is silently SKIPPED (advancing the cursor past a
        // lower .lo -> `r.hi <= cur` -> continue). That exact bug hid the focus
        // block for several commits once the higher-addressed 0x49B310 pool was
        // added ahead of it. Kept address-sorted here AND defensively sorted at
        // runtime so a future out-of-order insert can't re-break it.
        struct ExRange { uintptr_t lo, hi; };
        ExRange exr[] = {
            // Async window/mouse/focus block: Point (0x49AF04, mouse cursor,
            // 8B), __window_height/__window_width (0x49AF0C/10), and
            // g_focus_update_mask (0x49AF14 — window-focus state). All OS/window
            // async, not sim -> checksumming them false-desyncs whenever the
            // mouse/focus differs fwd-vs-resim (intermittent, mouse-dependent:
            // 0x9999/0x5555/0xBEEF/0x1234/0xFACE at distance=10). 0x14 covers it.
            { (uintptr_t)(0x49AF04_R), (uintptr_t)(0x49AF04_R) + 0x14 }, // Point + window dims + focus mask (async OS)
            // D3D11VertexBuffer NetworkNode pool struct (head @+0, block-list
            // fields follow, 0x20 bytes). RENDER resource pool: the per-frame
            // mesh update (update_renderable_mesh_and_transform ->
            // TF4::MeshVertex::StoreStreamWithStride -> real GPU CreateBuffer)
            // pops/pushes nodes forward-only, but the nodes live in a real-heap
            // region NOT covered by any snapshot. Rolling back the head (it sits
            // in th155 .data, so it WAS captured) restored it to a node forward
            // had since reused as a live D3D11VertexBuffer (first dword now the
            // refcount-obj vtable 0x842ACC) -> next pool pop writes v3[1]=1 to
            // read-only rdata -> the distance=10 round-2 WRITE crash at
            // th155+0x3AB1A. Exclude so the head stays forward-live. SIM pools in
            // this same table (Actor2D 0x49B370/390, Camera2D 0x49B410) MUST
            // still roll back -- do NOT widen this to a range.
            { (uintptr_t)(0x49B310_R), (uintptr_t)(0x49B310_R) + 0x20  }, // D3D11VertexBuffer pool (render, forward-only)
            { (uintptr_t)(0x4DAD10_R), (uintptr_t)(0x4DAD10_R) + 4    }, // _Wndproc window/input state
            { (uintptr_t)(0x4DAEB8_R), (uintptr_t)(0x4DAEB8_R) + 20   }, // DirectInput device buffer
            { (uintptr_t)(0x4DB004_R), (uintptr_t)(0x4DB004_R) + 4    }, // g_engine_loop_tick
            // bg-task condvar/mutex/thread block: originally [+0xA8) but the
            // block extends further — 0x4DB3C0 holds a WORKER THREAD ID (fwd
            // 0x7A8C vs re-sim -1) that flagged the f=347 false desync, and the
            // unnamed run up to g_ewctx_169c (0x4DB5A8) is all thread-pool
            // plumbing (netthread3 handle/id 0x4DB368/6C, cv wait state, ...)
            // written at wall-clock time by producer/consumer threads.
            { (uintptr_t)(0x4DB310_R), (uintptr_t)(0x4DB5A8_R)        }, // bg-task cv/mutex/thread block
            { (uintptr_t)(0x4DB6A8_R), (uintptr_t)(0x4DB6A8_R) + 16   }, // bg-ScriptAPI FIFO slot+count
            // RENDER SYNC BUFFERS (Manbow::*::SendToParentSync 0x55980 + world-rect
            // 0x9EF60, both forward-only render): per-player transform/camera state
            // sent to the render parent each frame. The headless re-sim never runs
            // that path, so these stay FROZEN at the restore value while the forward
            // pans the camera to follow the fighters -> any ASYMMETRIC matchup
            // flagged an instant eng "desync" (0x4DB790+0x40 AND 0x4DB8D0+0x40 =
            // buffer+0x40 position field; Dr0-confirmed writers 0x9F1E8 + 0x455CBA).
            // Mirror reimu/marisa never moved the camera so never exposed it. These
            // are pure presentation (gameplay reads none of it; sim camera state is
            // separately covered by the Camera2D/3D render-tainted pools). Buffer
            // layout from SendToParentSync: [1553]=0x4DB750 (24-dword stride x2),
            // [1649]=0x4DB8D0 (28-dword stride x2).
            { (uintptr_t)(0x4DB750_R), (uintptr_t)(0x4DB750_R) + 0xC0 }, // 24-dword sync buffers p0+p1
            { (uintptr_t)(0x4DB8D0_R), (uintptr_t)(0x4DB8D0_R) + 0xE0 }, // 28-dword sync buffers p0+p1
        };
        // DEFENSIVE: add_data requires exr ascending by .lo. Sort so an
        // out-of-order literal entry can never silently drop an exclusion again.
        {
            const int nexr = (int)(sizeof(exr) / sizeof(exr[0]));
            for (int i = 1; i < nexr; ++i) {
                ExRange k = exr[i]; int j = i - 1;
                while (j >= 0 && exr[j].lo > k.lo) { exr[j + 1] = exr[j]; --j; }
                exr[j + 1] = k;
            }
        }

        // Emit committed run [a,e) as snapshot region(s), carving out every exr[].
        auto add_data = [&](uintptr_t a, uintptr_t e) {
            uintptr_t cur = a;
            for (const auto& r : exr) {
                if (r.hi <= cur || r.lo >= e) continue;   // exclusion not in [cur,e)
                if (r.lo > cur) add((void*)cur, (uint32_t)(r.lo - cur));
                if (r.hi > cur) cur = r.hi;
                if (cur >= e) return;
            }
            if (cur < e) add((void*)cur, (uint32_t)(e - cur));
        };

        // MERGE contiguous committed pages into one run regardless of protection
        // (region_ok now walks, so a WRITECOPY->READWRITE boundary no longer
        // forces a split). This makes the region structure deterministic: it
        // depends only on the COMMITTED set (stable for the whole battle), not
        // on which pages happen to have been written -- killing the residual
        // eng/.data desync that was pure blob-structure noise.
        uintptr_t p = dstart, run = 0;
        int dseg = 0;
        while (p < dend) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((void*)p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            uintptr_t rend = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            bool committed = (mbi.State == MEM_COMMIT &&
                              !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)));
            if (committed) {
                if (!run) run = p;          // start/extend the committed run
            } else if (run) {
                add_data(run, p); run = 0; ++dseg;   // run ended at this gap
            }
            if (rend <= p) break;
            p = rend;
        }
        if (run) { add_data(run, dend); ++dseg; }
        static bool dlogged = false;
        if (!dlogged) {
            dlogged = true;
            log_printf("[engine_snap] .data snapshot: %d committed run(s) "
                       "(merged across protection)\n", dseg);
        }
    }

    // Act::ScriptAPI object + its four std::list sentinel nodes.
    void* sapi = *G_SCRIPTAPI_PTR;
    if (sapi) {
        add(sapi, SCRIPTAPI_BYTES);
        for (int i = 0; i < 4; ++i) {
            void* sentinel = *(void**)((uint8_t*)sapi + SAPI_LIST_OFF[i]);
            add(sentinel, SENTINEL_BYTES);
        }
    }

    // RunOneFrame driver: object, then its nested list. obj[0] -> the
    // container struct; container[0] -> the std::list ({_Myhead,_Mysize});
    // list[0] -> the sentinel node. The per-frame cursor lives inside the
    // object; the list's _Mysize and the sentinel's links mutate too.
    void* drv = *G_FRAMEDRV_PTR;
    if (drv) {
        add(drv, FRAMEDRV_BYTES);
        void* container = *(void**)drv;
        if (region_ok(container, 4)) {
            void* list = *(void**)container;
            if (region_ok(list, 8)) {
                add(list, 8);                       // _Myhead + _Mysize
                add(*(void**)list, SENTINEL_BYTES); // the sentinel node
            }
        }
    }

    // Determinism scalars.
    add(G_FRAME_COUNTER, 4);
    add(G_ACTOR_TASK_ID, 4);
    add(G_ACTOR_SERIAL_ID, 4);

    // CRT rand() seed — the global battle PRNG (see TH155_ACRT_GETPTD above).
    uintptr_t ptd = TH155_ACRT_GETPTD();
    if (ptd) add((void*)(ptd + CRT_HOLDRAND_OFF), 4);

    // MSVC C++ per-thread static-init epoch. Magic-static guards in th155
    // do `if (guard > *(*TLSP + 4))` to decide whether to run their init.
    // The epoch lives in TLS slot 0 (NOT the CRT ptd; that's a different
    // block holding rand etc.). Without this captured, a magic static that
    // runs during the forward pass advances the thread epoch; rollback
    // restores the guard but NOT the per-thread epoch, so the re-sim of
    // the same frame sees `guard <= epoch` and SKIPS the init — a
    // different code path that diverges the simulation (proven by
    // btDbvtBroadphase_createProxy firing only on forward, never re-sims).
    // TLS slot 0 (C++ thread-infra: static-init epoch, std::execution data, EH)
    // is RESTORE-BUT-NOT-CHECKSUM — captured in rng_collect below, NOT here.
    // It must restore (so the re-sim's static-init guards take the same branch
    // as forward) but must NOT fold into the desync checksum: at deep re-sim
    // depth this thread-plumbing state legitimately differs from forward (the
    // distance=10 TLS-heap desyncs, 0x2222 f=6 / 0xF00D f=818). Same class as
    // the Ew::sRandom SFMT state and STASK_FRAME.

    // DirectInput keyboard state table (see KBD_STATE_ADDR above).
    add(KBD_STATE_ADDR, KBD_STATE_BYTES);

    // NB: the Ew::sRandom SFMT19937 state is captured SEPARATELY (rng_save/
    // rng_load below), NOT here — it is RESTORE-BUT-NOT-CHECKSUM. Restoring it
    // gives the re-sim a consistent RNG start (fixes the sim-side effect-spawn
    // decision, i.e. the AnimController2D pointer divergence); but its end-of-
    // frame state is contaminated by render-tied effect draws (bitmapfont
    // damage numbers, layer-task particles — proven to spawn at diverging
    // counts fwd-vs-resim), so folding it into the desync checksum produces a
    // false eng divergence. Same class as cpp_arena: saved+restored, unchecked.

    // ENV gate: SQUIROLL_SNAP_EFFECT=0 disables the sEffect / sTask
    // additions from this snapshot. Use it to A/B test whether these
    // regions are implicated in a hang/freeze before reverting.
    static int s_snap_effect = -1;
    if (s_snap_effect < 0) {
        char buf[8] = {0};
        DWORD got = GetEnvironmentVariableA("SQUIROLL_SNAP_EFFECT", buf,
                                            sizeof(buf));
        s_snap_effect = (got == 0 || buf[0] != '0') ? 1 : 0;
        log_printf("[engine_snap] sEffect/sTask regions %s "
                   "(SQUIROLL_SNAP_EFFECT=%s)\n",
                   s_snap_effect ? "ENABLED" : "DISABLED",
                   got ? buf : "<unset>");
    }
    if (!s_snap_effect) return n;

    // Ew::sEffect — the effect-group manager singleton. We ONLY restore
    // the live-groups vector triple at +0xEC/+0xF0/+0xF4 (12 bytes).
    //
    // DO NOT snapshot the whole 0x218-byte object: it contains a Win32
    // HANDLE mutex at +0x4 (CreateMutexA in its ctor) and 16
    // std::shared_ptr<Concurrency::reader_writer_lock> slots populated
    // by the ctor. Restoring HANDLE bytes desyncs OS mutex state with
    // our "is-locked" view — observed as a hard freeze on hit when a
    // worker thread is mid-Wait/Release across a rollback save/load.
    //
    // The 12-byte triple is the only field that mutates per-frame in a
    // way that diverges from cpp_arena-restored state; the rest of
    // sEffect (locks, shared_ptrs, counters) is set up at boot and
    // persistent for the battle.
    if (void* seffect = *G_EW_SEFFECT) {
        add((uint8_t*)seffect + 0xEC, 12);  // begin/end/cap
    }

    // Ew::sTask — the job-scheduler singleton (0x1AAC8 bytes total).
    // Same surgical approach: only the per-frame counters around
    // +0x18014, the 32 per-layer vector triples, and the status/timing
    // flags near +0x1AAC0. Layer task-member objects live in cpp_arena
    // and ride that snapshot. The mutex at sTask+0x4 and the
    // shared_ptrs at sTask+0x28.. are deliberately NOT touched.
    if (void* stask = *G_EW_STASK) {
        // STASK_FRAME (+0x18014) is the worker-frame head counter — incremented
        // by DispatchWorkerTask (render/worker path) FORWARD-ONLY, frozen in the
        // headless re-sim -> false eng desync on effect-spawning matchups (0x9999
        // f=2). Captured via rng_collect (restore-but-not-checksum) instead, so
        // it still restores but never checksums. The LAYER triples + FLAGS are
        // real sim state and STAY checksummed (moving them hung 0xBEEF/0xF00D).
        add((uint8_t*)stask + STASK_LAYER_BASE_OFF,
            STASK_LAYER_COUNT * STASK_LAYER_TRIPLE);
        add((uint8_t*)stask + STASK_FLAGS_OFF, STASK_FLAGS_BYTES);
    }

    return n;
}

} // namespace

// Diagnosis helper: classify a diverging captured address (from [engtrip]) as
// a named singleton+offset so a per-real-frame counter can be identified and
// moved to restore-but-not-checksum (like STASK_FRAME).
void classify_and_log(uint32_t addr, uint32_t fwd, uint32_t resim) {
    uint32_t se = (uint32_t)(uintptr_t)(*G_EW_SEFFECT);
    uint32_t st = (uint32_t)(uintptr_t)(*G_EW_STASK);
    uint32_t sa = (uint32_t)(uintptr_t)(*G_SCRIPTAPI_PTR);
    const char* what = "?"; uint32_t off = 0;
    if (se && addr >= se && addr < se + 0x218)       { what = "sEffect";   off = addr - se; }
    else if (st && addr >= st && addr < st + 0x1AAC8){ what = "sTask";     off = addr - st; }
    else if (sa && addr >= sa && addr < sa + 0x108)  { what = "ScriptAPI"; off = addr - sa; }
    else if (addr >= 0x400000 && addr < 0x800000)    { what = ".data";     off = addr; }
    log_printf("[engclass] diverging addr=0x%08X = %s+0x%X  fwd=%08X resim=%08X  "
               "(seffect=%08X stask=%08X scriptapi=%08X)\n",
               addr, what, off, fwd, resim, se, st, sa);
}

// CACHED region list (see rng_collect note): collect() resolves ~50 regions,
// VirtualQuery-validating each every save (~8ms — the .data committed-run walk
// + pointer-graph region_ok). The region ADDRESSES are stable for the whole
// battle (fixed .data, singletons allocated once, pool structs fixed), so
// resolve ONCE and reuse; only the byte copy (~293KB, ~30us) runs per save.
static Region g_col_cache[MAX_REGIONS];
static int    g_col_cached = -1;
void rng_reset_cache();   // fwd (defined below) — also resets this cache

uint32_t save(uint8_t* out, uint32_t cap) {
    Region r[MAX_REGIONS];
    int n;
    if (g_col_cached >= 0) {
        n = g_col_cached;
        for (int i = 0; i < n; ++i) r[i] = g_col_cache[i];
    } else {
        n = collect(r);
        // Latch once the pointer graph is up (ScriptAPI/sEffect/sTask resolved);
        // by the first post-arm save the battle is fully built, so n is stable.
        if (n > 0) { for (int i = 0; i < n; ++i) g_col_cache[i] = r[i]; g_col_cached = n; }
    }

    uint8_t* p   = out;
    uint8_t* end = out + cap;
    auto put = [&](const void* src, uint32_t l) -> bool {
        if (p + l > end) return false;
        memcpy(p, src, l);
        p += l;
        return true;
    };

    uint32_t magic = SNAP_MAGIC, count = (uint32_t)n;
    if (!put(&magic, 4) || !put(&count, 4)) return 0;
    for (int i = 0; i < n; ++i) {
        uint32_t a = (uint32_t)(uintptr_t)r[i].addr;
        if (!put(&a, 4) || !put(&r[i].len, 4) || !put(r[i].addr, r[i].len))
            return 0;
    }
    return (uint32_t)(p - out);
}

// Ew::sRandom SFMT19937 state — RESTORE-BUT-NOT-CHECKSUM (see collect() note).
// Chase g_ew_random_mgr(0x4DB0C4) -> vector<cRandom*> -> cRandom[1] wrapper ->
// wrapper[1] = 0x9C8-byte state; serialize each as (addr,len,bytes) so rng_load
// restores by saved address. The buffers are aligned_malloc'd once at init and
// never move, so the address is a stable restore target.
// Collect the RESTORE-BUT-NOT-CHECKSUM regions as (addr,len): SFMT RNG state
// buffers + the sTask worker-frame counter (forward-only). Restored so the
// re-sim is consistent, but excluded from the desync checksum (their bytes are
// advanced forward-only by the render/worker path).
struct NcRegion { uint32_t addr, len; };
// CACHED: the SFMT state buffers + sTask frame counter are aligned_malloc'd /
// heap-allocated ONCE at battle init and never move. Re-chasing the pointer
// graph and VirtualQuery-validating every region on EVERY save cost ~24ms/save
// (the dominant sblob cost). Resolve once (first save where the RNG exists),
// cache the (addr,len) list, and just copy from it thereafter. A new battle
// re-arms the snapshot which resets g_nc_cached, so stale addresses can't leak.
static NcRegion g_nc_cache[32];
static int      g_nc_cached = -1;   // -1 = not resolved yet
static int rng_collect(NcRegion* r, int maxn) {
    if (g_nc_cached >= 0) {          // fast path: reuse the resolved list
        int n = g_nc_cached < maxn ? g_nc_cached : maxn;
        for (int i = 0; i < n; ++i) r[i] = g_nc_cache[i];
        return n;
    }
    int n = 0;
    void* mgr = *(void**)(0x4DB0C4_R);
    if (mgr && region_ok(mgr, 12)) {
        uint32_t begin = ((uint32_t*)mgr)[0], end = ((uint32_t*)mgr)[1];
        for (uint32_t p = begin; p && p + 4 <= end && n < maxn; p += 4) {
            uint32_t crandom = *(uint32_t*)(uintptr_t)p;
            if (!crandom || !region_ok((void*)(uintptr_t)crandom, 8)) continue;
            uint32_t wrapper = *(uint32_t*)(uintptr_t)(crandom + 4);
            if (!wrapper || !region_ok((void*)(uintptr_t)wrapper, 8)) continue;
            uint32_t state = *(uint32_t*)(uintptr_t)(wrapper + 4);
            if (state && region_ok((void*)(uintptr_t)state, 0x9C8))
                r[n++] = { state, 0x9C8 };
        }
    }
    if (void* stask = *G_EW_STASK) {
        uint32_t a = (uint32_t)(uintptr_t)stask + STASK_FRAME_OFF;
        if (n < maxn && region_ok((void*)(uintptr_t)a, STASK_FRAME_BYTES))
            r[n++] = { a, STASK_FRAME_BYTES };   // worker-frame head counter
    }
    // TLS slot 0 (see note in collect()): restore the sim thread's C++
    // thread-infra chunk so re-sim static-init guards match forward, but keep
    // it out of the checksum. rng_collect runs on the sim thread (save/restore
    // path), so __readfsdword reads the sim thread's TEB; the address is stable
    // per-match and safely cached with the rest.
    {
        void** tlsa = (void**)__readfsdword(0x2C);
        if (tlsa && tlsa[0] && n < maxn && region_ok(tlsa[0], 256))
            r[n++] = { (uint32_t)(uintptr_t)tlsa[0], 256 };
    }
    // Cache only once the graph is fully built (RNG present) so we don't latch
    // an empty list during boot. Until then, resolve every call (cheap: empty).
    if (n > 0) {
        for (int i = 0; i < n; ++i) g_nc_cache[i] = r[i];
        g_nc_cached = n;
    }
    return n;
}
// Called from snapshot_ring::arm() (via engine_snap) at each battle start so a
// fresh match re-resolves the RNG/sTask addresses.
void rng_reset_cache() { g_nc_cached = -1; g_col_cached = -1; }

uint32_t rng_save(uint8_t* out, uint32_t cap) {
    NcRegion r[24];
    int n = rng_collect(r, 24);
    uint8_t* p = out; uint8_t* e = out + cap;
    auto put = [&](const void* s, uint32_t l) -> bool {
        if (p + l > e) return false; memcpy(p, s, l); p += l; return true;
    };
    uint32_t magic = SNAP_MAGIC ^ 0x524E47u, cnt = (uint32_t)n;
    if (!put(&magic, 4) || !put(&cnt, 4)) return 0;
    for (int i = 0; i < n; ++i)
        if (!put(&r[i].addr, 4) || !put(&r[i].len, 4) ||
            !put((void*)(uintptr_t)r[i].addr, r[i].len)) return 0;
    return (uint32_t)(p - out);   // never 0: 8-byte header stands even when empty
}

void rng_load(const uint8_t* blob, uint32_t len) {
    if (len < 8) return;
    const uint8_t* p = blob; const uint8_t* e = blob + len;
    uint32_t magic = 0, cnt = 0;
    memcpy(&magic, p, 4); p += 4; memcpy(&cnt, p, 4); p += 4;
    if (magic != (SNAP_MAGIC ^ 0x524E47u)) return;
    for (uint32_t i = 0; i < cnt && p + 8 <= e; ++i) {
        uint32_t a = 0, l = 0;
        memcpy(&a, p, 4); p += 4; memcpy(&l, p, 4); p += 4;
        if (p + l > e) return;
        // No region_ok: save-validated, battle-stable addresses (see load()).
        if (a && l && l <= 0x9C8) memcpy((void*)(uintptr_t)a, p, l);
        p += l;
    }
}

void load(const uint8_t* blob, uint32_t len) {
    if (len < 8) return;
    const uint8_t* p   = blob;
    const uint8_t* end = blob + len;

    uint32_t magic = 0, count = 0;
    memcpy(&magic, p, 4); p += 4;
    memcpy(&count, p, 4); p += 4;
    if (magic != SNAP_MAGIC) {
        log_printf("[engine_snap] load: bad magic %08x\n", magic);
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (p + 8 > end) return;
        uint32_t a = 0, l = 0;
        memcpy(&a, p, 4); p += 4;
        memcpy(&l, p, 4); p += 4;
        if (p + l > end) return;
        // No region_ok here: these addresses came from THIS battle's save(),
        // which region_ok-validated them (and they are stable — fixed .data +
        // singletons allocated once), so per-load VirtualQuery was pure overhead
        // (~5.5ms/load). A stale-address blob can't reach us: the cache resets at
        // arm and gekko only restores same-match frames.
        if (a) memcpy((void*)(uintptr_t)a, p, l);
        p += l;
    }
}

void diff_locate(int frame, int rb) {
    // Diffs the actual save() BLOB forward-vs-resim, region by region, so it
    // covers BOTH the .data sub-regions (minus exclusions) AND the extra
    // captured objects (ScriptAPI lists, framedrv, seffect/stask, kbd). The
    // blob is self-describing: [magic][count] then per region [addr][len][bytes].
    static constexpr int      RING = 16;
    static constexpr uint32_t CAP  = 0x80000;   // 512K headroom for the eng blob
    static uint8_t* ring[RING] = {};
    static uint32_t rlen[RING] = {};
    static int      rframe[RING];
    static uint8_t* tmp = nullptr;
    static bool     init = false;
    static int      budget = 120;
    if (!init) { init = true; for (int i = 0; i < RING; ++i) rframe[i] = -1; }
    if (frame < 0) return;

    int slot = ((frame % RING) + RING) % RING;
    if (!ring[slot]) {
        ring[slot] = (uint8_t*)VirtualAlloc(nullptr, CAP,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!ring[slot]) return;
    }
    if (rb == 0) {                              // forward: store the baseline blob
        rlen[slot] = save(ring[slot], CAP);
        rframe[slot] = frame;
        return;
    }
    if (rframe[slot] != frame || budget <= 0) return;
    if (!tmp) {
        tmp = (uint8_t*)VirtualAlloc(nullptr, CAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!tmp) return;
    }
    uint32_t nlen = save(tmp, CAP);
    const uint8_t* a = ring[slot];
    const uint8_t* b = tmp;
    if (rlen[slot] == 0 || nlen == 0) return;
    if (rlen[slot] != nlen) {
        // Region SET changed (a conditionally-captured region appeared or
        // vanished). Walk both region tables in lockstep (collect() emits them
        // in a fixed order) and report the first region whose (addr,len) header
        // differs -> names the added/removed region.
        uint32_t ca = *(const uint32_t*)(a + 4);
        uint32_t cb = *(const uint32_t*)(b + 4);
        const uint8_t* pa = a + 8;
        const uint8_t* pb = b + 8;
        uint32_t nmin = ca < cb ? ca : cb;
        for (uint32_t r = 0; r < nmin; ++r) {
            uint32_t aa = *(const uint32_t*)pa, al = *(const uint32_t*)(pa + 4);
            uint32_t ba = *(const uint32_t*)pb, bl = *(const uint32_t*)(pb + 4);
            if (aa != ba || al != bl) {
                log_printf("[engdiff] f=%d region[%u] STRUCT diff: "
                           "fwd=%08X(rva %08X)/len%u  now=%08X(rva %08X)/len%u\n",
                           frame, r, aa, aa - (uint32_t)base_address, al,
                           ba, ba - (uint32_t)base_address, bl);
                break;
            }
            pa += 8 + al; pb += 8 + bl;
        }
        log_printf("[engdiff] f=%d blob LEN fwd=%u now=%u  nreg fwd=%u now=%u\n",
                   frame, rlen[slot], nlen, ca, cb);
        --budget; return;
    }
    uint32_t count = *(const uint32_t*)(a + 4);
    const uint8_t* pa = a + 8;
    const uint8_t* pb = b + 8;
    int hits = 0;
    for (uint32_t r = 0; r < count && hits < 10; ++r) {
        uint32_t addr = *(const uint32_t*)pa;
        uint32_t len  = *(const uint32_t*)(pa + 4);
        const uint8_t* da = pa + 8;
        const uint8_t* db = pb + 8;
        for (uint32_t i = 0; i + 4 <= len && hits < 10; i += 4) {
            uint32_t fv = *(const uint32_t*)(da + i);
            uint32_t nv = *(const uint32_t*)(db + i);
            if (fv != nv) {
                ++hits;
                log_printf("[engdiff] f=%d region=%08X(rva %08X)+0x%X "
                           "fwd=%08X now=%08X\n", frame, addr,
                           addr - (uint32_t)base_address, i, fv, nv);
            }
        }
        pa += 8 + len;
        pb += 8 + len;
    }
    if (hits) --budget;
    else      log_printf("[engdiff] f=%d rb=%d eng blob IDENTICAL\n", frame, rb);
}

} // namespace engine_snap
