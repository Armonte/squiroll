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
// Healthy boost::signals2 grouped_list connection counts are O(100).
// Set the cap well above any realistic count but well below "infinite"
// so a corrupted-cycle walk bails in milliseconds.
static constexpr uint32_t kWalkMaxSteps = 100000;

namespace {

static SafetyHookInline g_h{};
static SafetyHookInline g_h_walk{};
static std::atomic<uint64_t> g_walk_bailouts{0};
static std::atomic<uint64_t> g_walk_total{0};

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

} // namespace

void install() {
    g_h = safetyhook::create_inline((void*)CONCURRENT_LIST_ITER_STEP,
                                    (void*)hook);
    g_h_walk = safetyhook::create_inline((void*)CONCURRENT_LIST_WALK_VISIT,
                                         (void*)walk_hook);
    log_printf("[clguard] hook concurrent_list_iter_step @ 0x%X %s, "
               "concurrent_list_walk_visit @ 0x%X %s (cap=%u)\n",
               (uint32_t)CONCURRENT_LIST_ITER_STEP,
               g_h.enabled() ? "OK" : "FAIL",
               (uint32_t)CONCURRENT_LIST_WALK_VISIT,
               g_h_walk.enabled() ? "OK" : "FAIL",
               kWalkMaxSteps);
}

} // namespace cl_iter_guard
