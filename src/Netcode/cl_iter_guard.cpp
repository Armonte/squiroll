// safetyhook MUST be included before any squiroll header.
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>

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
#define CONCURRENT_LIST_ITER_STEP (0x134D0_R)

namespace {

static SafetyHookInline g_h{};

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

} // namespace

void install() {
    g_h = safetyhook::create_inline((void*)CONCURRENT_LIST_ITER_STEP,
                                    (void*)hook);
    log_printf("[clguard] hook concurrent_list_iter_step @ 0x%X %s\n",
               (uint32_t)CONCURRENT_LIST_ITER_STEP,
               g_h.enabled() ? "OK" : "FAIL");
}

} // namespace cl_iter_guard
