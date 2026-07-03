// safetyhook MUST be included before any squiroll header.
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <atomic>

#include "patch_utils.h"
#include "util.h"          // thiscall
#include "log.h"
#include "cpp_arena.h"
#include "sq_arena.h"
#include "bullet_arena.h"
#include "crash_handler.h" // watchpoint_arm — corruptor hunt
#include "sync_pin.h"      // pin() — register inlined-ctor mutexes on first use

namespace gekko_bridge { extern int g_trace_rb; extern int g_trace_frame; }

namespace cl_iter_guard {

// concurrent_list_iter_step at 0x134D0. Reads *(this_+2) as iter position,
// passes it (or a derivative) to concurrent_list_walk_visit. If that field
// holds an arena base address (a known runtime constant) then walk_visit
// dereferences it as a list node and faults. We catch this UPSTREAM: short-
// circuit iter_step when the iter pos is one of our arena bases, before
// walk_visit even runs.
//
// iter_insert (the immediate caller) IGNORES iter_step's return value, so
// returning null is safe (no node link missed, the actual insert still
// happens via operator new + sub_13400 + sub_18EF0 in iter_insert).
#define CONCURRENT_LIST_ITER_STEP  (0x134D0_R)
// concurrent_list_walk_visit at 0x13B00. Walks the linked list starting
// from `*(this[2])`, calling sub_1AD10 on each `+12` flagged node, and
// invoking concurrent_list_erase_node on unmarked ones. Loop exit is
// "current node's next-pointer wraps back to head". If a re-sim leaves
// the boost::signals2 grouped_list in a state where a node's `next`
// points into the middle of the list instead of back to head, this
// walk never terminates and the game freezes — observed during the
// rollback re-sim of Actor2D::ConnectRenderSlot via watchpoint at
// 0x13B86 (the function tail).
//
// Defence: cap the visited count via the existing `a4` (max_visits)
// argument we don't actually use, OR add our own counter. We do the
// latter so the original signature is preserved.
#define CONCURRENT_LIST_WALK_VISIT (0x13B00_R)
// call_boost__function_3 (0x32400): invokes a boost::signals2 slot's
// boost::function. Its empty-function path (*this == 0) throws
// std::bad_function_call, which is UNCAUGHT -> abort. Under leak-on-free a
// slot whose connection node was freed is left zeroed, so a re-sim that walks
// a not-yet-cleaned-up (dangling) connection invokes an empty function and
// aborts — the intermittent re-sim crash. A dead slot must NOT run, so we skip
// it (return 0), the same philosophy as the universal null-deref skip.
#define CALL_BOOST_FUNCTION_3 (0x32400_R)
// sound_stream_write_chunk_slot (0x16C060): the game-loop signal slot that
// pumps decoded BGM chunks into the stream's circular buffer, guarded by a
// std::mutex at writer+16 that the AUDIO thread locks concurrently from the
// read side. Two rollback hazards, both observed as the f~60/f~248 FASTFAILs
// and the intermittent mid-round stall:
//  1. That mutex's ctor is INLINED by the LTCG static CRT, so sync_pin's
//     _Mtx_init_in_situ hook never sees it -> not pinned -> a restore rewinds
//     a lock the audio thread is actively using -> _Mtx_lock error ->
//     _Throw_C_error -> uncaught -> abort. Fix: self-healing pin on first use.
//  2. Re-sims re-pump the same chunks (audio is a wall-clock side effect, not
//     sim state): buffer fills -> writer blocks at real-time drain speed (the
//     [runone] rb=1 stall), and dead streams re-release their resource handle
//     every re-sim (refcount underflow). Fix: skip the pump when rb != 0.
#define SOUND_STREAM_WRITE_CHUNK_SLOT (0x16C060_R)
// remove_id_from_buffer_threadsafe (0xECF40): WaitForSingleObject(mutex@this+12,
// INFINITE) -> scan/remove a 32-bit id from the manager's buffer -> ReleaseMutex.
// Called from the effect-particle destructor during Ew_sTask::StepLayers32. Under
// deep rollback, the re-sim can destroy a particle whose group/manager DIVERGED
// (StepLayers32's IDA note: the group's +0x308 ref to a destroyed prim is not
// cleared -> stale group ref), so `this` points at a stale/contended manager and
// the INFINITE wait never returns -> the rare distance=10 hang (sim thread parked
// here, g_rollback_cs free). Guard: during re-sim only, probe the mutex with a
// short timeout; if it can't be taken (the divergent case), SKIP the removal
// rather than block forever. Win32 mutexes are recursive, so on success we own it
// and the original's own acquire returns immediately -> zero behaviour change in
// the normal (free-mutex) case; a genuine stuck mutex becomes a skipped op (at
// worst a detectable desync) instead of a 20s freeze.
#define REMOVE_ID_FROM_BUFFER (0xECF40_R)
// update_effect_state (0x10B810, sole caller Ew_tEftElect vtable[2] worker job,
// dispatched via Ew_sTask::DispatchWorkerTask under SYNC_WORKERS): reads the
// state's LINKED PARTICLE ptr at this+4 and calls through its vtable (+0x1C).
// Vanilla ordering guarantees the job never runs after the particle detaches
// (Ew_tEftParticle::DetachFromOwnerGroup zeroes member back-ptrs); a rollback
// re-sim replays the job against a state whose linked particle was already
// detached/NULLed -> NULL+0x1C deref -> the clguard universal skip then turned
// it into EXEC-at-NULL + a wild WRITE (the 0xEAC9 round-end crash, captured
// x_crash_1). Guard: NULL linked particle -> return 0 ("effect terminated"),
// which is semantically what a detached effect is.
#define UPDATE_EFFECT_STATE (0x10B810_R)
// Healthy boost::signals2 grouped_list connection counts are O(100).
// Set the cap well above any realistic count but well below "infinite"
// so a corrupted-cycle walk bails in milliseconds.
static constexpr uint32_t kWalkMaxSteps = 100000;

namespace {

static SafetyHookInline g_h{};
static SafetyHookInline g_h_walk{};
static SafetyHookInline g_h_callfn{};
static SafetyHookInline g_h_sndslot{};
static SafetyHookInline g_h_removeid{};
static SafetyHookInline g_h_updeff{};
static std::atomic<uint64_t> g_updeff_skips{0};
static std::atomic<uint64_t> g_walk_bailouts{0};
static std::atomic<uint64_t> g_walk_total{0};
static std::atomic<uint64_t> g_callfn_skips{0};
static std::atomic<uint64_t> g_sndslot_skips{0};
static std::atomic<uint64_t> g_removeid_skips{0};

static inline bool is_arena_base(uintptr_t v) {
    if (!v) return false;
    if ((uintptr_t)cpp_arena::base()    == v) return true;
    if ((uintptr_t)sq_arena::base()     == v) return true;
    if ((uintptr_t)bullet_arena::base() == v) return true;
    return false;
}

// __thiscall(this, a2) -> int*. Plain — no extra stack arg dance.
static int* thiscall hook(int* this_, int a2) {
    if (this_) {
        uintptr_t iter_pos = (uintptr_t)(uint32_t)this_[2];

        // (Previously: corruptor-hunt DR0 arm on first heap list_state.
        // Removed — the hits are all legitimate task-list writes by
        // Act::ScriptAPI::RunOneFrame; arming on any one list rarely
        // catches the corruptor since each run picks a different
        // affected list. Hunt continues via the IDA route.)

        if (is_arena_base(iter_pos)) {
            static uint32_t hits = 0;
            if (((hits++) & 0x3F) == 0) {
                log_printf("[clguard] iter_step short-circuit: this=%p "
                           "iter_pos=%08X is an arena base; skipping walk "
                           "(hit #%u)\n",
                           this_, (uint32_t)iter_pos, hits);
            }
            return nullptr;
        }
    }
    return g_h.unsafe_thiscall<int*>(this_, a2);
}

// __thiscall(this, a2, a3, a4_node_in_out, a5_max_visits) -> int*
//
// The original's a5 (max_visits) appears to be 0 in every call site we
// see — so the walk is unbounded by default. We intercept and supply
// our own cap if the caller's cap is missing or unreasonable. Calling
// the original with our cap keeps semantics intact for healthy lists
// (the cap will never be reached); for corrupted ones the walk bails
// at kWalkMaxSteps instead of spinning forever.
//
// arg layout per the asm:
//   ecx = this (list root)
//   [ebp+arg_0] = arg_0  (this_chain_state, used by sub_1AD10)
//   [ebp+arg_4] = arg_4  (one-byte flag)
//   [ebp+arg_8] = arg_8  (in/out node ptr)
//   [ebp+arg_C] = arg_C  (max visits — set to 0 by default)
static int* thiscall walk_hook(int* this_, int a2, char a3, int* a4,
                               unsigned int a5) {
    g_walk_total.fetch_add(1, std::memory_order_relaxed);
    unsigned int cap = (a5 == 0 || a5 > kWalkMaxSteps) ? kWalkMaxSteps : a5;
    int* result = g_h_walk.unsafe_thiscall<int*>(this_, a2, a3, a4, cap);
    return result;
}

// __thiscall(this) -> int. `this` is the boost::function object; this[0] is the
// vtable/target descriptor (0 == empty). Skip an empty (dead/freed) slot rather
// than let the original throw bad_function_call -> abort. A valid slot (target
// != 0) goes straight through — one predicated load of overhead.
static int thiscall callfn_hook(int* this_) {
    // Skip a dead/corrupt slot: target (this_[0]) is either 0 (empty function ->
    // bad_function_call throw) or a small garbage int left by a freed connection
    // (e.g. 0xC, so (target&~1)+4 = 0x10 -> the observed fault=0x10 read). Any
    // target below the 64KB null-reserve is not a real descriptor pointer.
    if (this_ && (uintptr_t)this_ >= 0x10000) {
        uint32_t target = (uint32_t)this_[0];
        if (target < 0x10000) {
            uint64_t n = g_callfn_skips.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & 0x3FF) == 1)
                log_printf("[slotguard] skipped dead boost::function slot this=%p "
                           "target=%08X (#%llu)\n",
                           this_, target, (unsigned long long)n);
            return 0;
        }
    }
    return g_h_callfn.unsafe_thiscall<int>(this_);
}

// __cdecl(int* pwriter, DWORD* hres). See SOUND_STREAM_WRITE_CHUNK_SLOT above.
static void cdecl sndslot_hook(int* pwriter, uint32_t* hres) {
    uint32_t writer = pwriter ? (uint32_t)*pwriter : 0;
    if (writer >= 0x10000) {
        // Self-healing sync-pin: the writer's std::mutex imp at +16 (ctor is
        // CRT-inlined so the init hook missed it). pin() dedupes by address.
        sync_pin::pin(writer + 16, 0x30);
        static uint32_t pinned_once = 0;
        if (pinned_once != writer) {
            pinned_once = writer;
            log_printf("[sndguard] pinned stream-writer mutex %08X (+16)\n",
                       writer);
        }
    }
    if (gekko_bridge::g_trace_rb) {   // re-sim: audio is wall-clock, don't pump
        g_sndslot_skips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_h_sndslot.unsafe_ccall<void>(pwriter, hres);
}

// __thiscall(this) -> bool "still active". See UPDATE_EFFECT_STATE above.
static char thiscall updeff_hook(int self) {
    if (self >= 0x10000 && *(uint32_t*)(uintptr_t)(self + 4) == 0) {
        uint64_t n = g_updeff_skips.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 4 || (n & 0x3F) == 0)
            log_printf("[updeff] NULL linked particle this=%08X f=%d rb=%d -> "
                       "terminated (#%llu)\n", (uint32_t)self,
                       gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb,
                       (unsigned long long)n);
        return 0;   // detached effect = done; caller reaps it cleanly
    }
    return g_h_updeff.unsafe_thiscall<char>(self);
}

// __thiscall(this, target_id) -> BOOL. See REMOVE_ID_FROM_BUFFER above.
static int thiscall removeid_hook(int self, int target_id) {
    if (gekko_bridge::g_trace_rb && self >= 0x10000) {
        HANDLE h = *(HANDLE*)(uintptr_t)(self + 12);
        if (h) {
            // Probe with a short timeout (free mutex -> returns instantly, so no
            // behaviour change; a stuck/stale-manager mutex times out -> skip).
            DWORD r = WaitForSingleObject(h, 30);
            if (r == WAIT_TIMEOUT) {
                uint64_t n = g_removeid_skips.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((n & 0x3F) == 1)
                    log_printf("[removeid] re-sim skip: mutex %p stuck (stale "
                               "effect manager) this=%08X (#%llu)\n",
                               h, (uint32_t)self, (unsigned long long)n);
                return 0;   // skip the buffer removal rather than block forever
            }
            // We now own the (recursive) mutex; the original re-acquires it
            // without blocking, does the removal, releases once. Release our
            // extra hold afterward so the net count is unchanged.
            int ret = g_h_removeid.unsafe_thiscall<int>(self, target_id);
            ReleaseMutex(h);
            return ret;
        }
    }
    return g_h_removeid.unsafe_thiscall<int>(self, target_id);
}

} // namespace

void install() {
    g_h = safetyhook::create_inline((void*)CONCURRENT_LIST_ITER_STEP,
                                    (void*)hook);
    g_h_walk = safetyhook::create_inline((void*)CONCURRENT_LIST_WALK_VISIT,
                                         (void*)walk_hook);
    g_h_callfn = safetyhook::create_inline((void*)CALL_BOOST_FUNCTION_3,
                                           (void*)callfn_hook);
    g_h_sndslot = safetyhook::create_inline((void*)SOUND_STREAM_WRITE_CHUNK_SLOT,
                                            (void*)sndslot_hook);
    g_h_removeid = safetyhook::create_inline((void*)REMOVE_ID_FROM_BUFFER,
                                             (void*)removeid_hook);
    g_h_updeff = safetyhook::create_inline((void*)UPDATE_EFFECT_STATE,
                                           (void*)updeff_hook);
    log_printf("[clguard] hook concurrent_list_iter_step @ 0x%X %s, "
               "concurrent_list_walk_visit @ 0x%X %s (cap=%u), "
               "call_boost__function_3 @ 0x%X %s, "
               "sound_stream_write_chunk_slot @ 0x%X %s\n",
               (uint32_t)CONCURRENT_LIST_ITER_STEP,
               g_h.enabled() ? "OK" : "FAIL",
               (uint32_t)CONCURRENT_LIST_WALK_VISIT,
               g_h_walk.enabled() ? "OK" : "FAIL",
               kWalkMaxSteps,
               (uint32_t)CALL_BOOST_FUNCTION_3,
               g_h_callfn.enabled() ? "OK" : "FAIL",
               (uint32_t)SOUND_STREAM_WRITE_CHUNK_SLOT,
               g_h_sndslot.enabled() ? "OK" : "FAIL");
    log_printf("[clguard] remove_id_from_buffer_threadsafe @ 0x%X %s, "
               "update_effect_state @ 0x%X %s\n",
               (uint32_t)REMOVE_ID_FROM_BUFFER,
               g_h_removeid.enabled() ? "OK" : "FAIL",
               (uint32_t)UPDATE_EFFECT_STATE,
               g_h_updeff.enabled() ? "OK" : "FAIL");
}

} // namespace cl_iter_guard
