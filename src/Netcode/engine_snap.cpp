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
static constexpr int      MAX_REGIONS = 24;

// True if [p, p+len) lies entirely in committed, accessible memory.
// Resolving the scheduler graph walks reverse-engineered struct offsets;
// validating every region turns a wrong offset into a skipped region
// (-> a detectable desync) instead of a crash.
static bool region_ok(const void* p, uint32_t len) {
    if (!p || len == 0) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return (uintptr_t)p + len <= region_end;
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
    add((void*)(0x498000_R), 0x47AA4);

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
    {
        // TIB layout (32-bit): offset 0x2C = ThreadLocalStoragePointer.
        // Capture a generous chunk of TLS slot 0 to cover thread-local state
        // (C++ static-init epoch, std::execution thread data, ...) regardless
        // of exact field offsets in this MSVC build. region_ok in add()
        // bounds-checks; an over-large add is safely truncated.
        void** tlsa = (void**)__readfsdword(0x2C);
        if (tlsa && tlsa[0]) add(tlsa[0], 256);
    }

    // DirectInput keyboard state table (see KBD_STATE_ADDR above).
    add(KBD_STATE_ADDR, KBD_STATE_BYTES);

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
        add((uint8_t*)stask + STASK_FRAME_OFF, STASK_FRAME_BYTES);
        add((uint8_t*)stask + STASK_LAYER_BASE_OFF,
            STASK_LAYER_COUNT * STASK_LAYER_TRIPLE);
        add((uint8_t*)stask + STASK_FLAGS_OFF, STASK_FLAGS_BYTES);
    }

    return n;
}

} // namespace

uint32_t save(uint8_t* out, uint32_t cap) {
    Region r[MAX_REGIONS];
    int n = collect(r);

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
        void* addr = (void*)(uintptr_t)a;
        if (region_ok(addr, l)) memcpy(addr, p, l);
        p += l;
    }
}

} // namespace engine_snap
