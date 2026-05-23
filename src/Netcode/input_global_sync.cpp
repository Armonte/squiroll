// safetyhook MUST be included before any squiroll header — util.h #defines
// the calling-convention keywords as attribute macros and safetyhook uses
// those identifiers as method names.
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "patch_utils.h"   // _R address literal
#include "util.h"          // thiscall
#include "log.h"

// gekko_bridge.cpp publishes these (sq_trace pattern).
namespace gekko_bridge { extern int g_trace_frame; extern int g_trace_rb; }

namespace input_global_sync {

// =============================================================================
// FIX for the rollback divergence root cause documented in
// project_squiroll_tf4_mspace.md: Manbow::InputCommand reads its per-frame
// input bits via *(this+0)->vtable[4](this) which returns a pointer to 8
// ints starting at source+4. Those 8 ints are written somewhere upstream
// (Manbow::InputMulti::Update + an unidentified writer that runs between
// InputMulti::Update returning and InputCommand::Update reading), and the
// write is non-deterministic across re-sims.
//
// First attempt: capture-replay at Manbow::InputMulti::Update entry — see
// the run that produced a CAP log of all zeros even though InputCommand
// later read [FFFFFFFF, 0, ...]. Some other code writes source+4 BETWEEN
// InputMulti::Update returning and InputCommand::Update reading, so the
// InputMulti-level capture is stale.
//
// Second attempt (this file): capture-replay at Manbow::InputCommand::Update
// entry — RIGHT before InputCommand reads source+4 via vtable[4]. The
// capture is taken on the FORWARD run, the restore is taken on every
// re-sim. By the time our hook runs the read in re-sim, *(source+4..) is
// guaranteed identical to what forward saw, regardless of who the other
// writer is — we don't have to find them.
//
// Per-frame ring keyed by (frame, InputCommand pointer): 2 instances live
// (one per player), so INSTANCES=4 is over-provisioned.
// =============================================================================

#define INPUTCOMMAND_UPDATE (0x74390_R)

namespace {

static SafetyHookInline g_h{};

// InputCommand reads 8 ints from source+4. We capture & restore exactly
// those 32 bytes — the read range is tight (vtable[4] returns source+4,
// InputCommand reads v3[0..7]).
static constexpr uint32_t STATE_BYTES = 32;

// Per-frame, per-instance ring. Gekko rollback window = 8 frames; 16 frames
// gives headroom. We see exactly 2 InputCommand instances live per frame
// (one per player); 8 buys headroom for any future variant.
static constexpr uint32_t RING        = 16;
static constexpr uint32_t INSTANCES   = 8;

struct Slot {
    int       frame;                 // gekko frame this state was captured at
    void*     inst;                  // the InputMulti pointer; key
    uint8_t   bytes[STATE_BYTES];    // captured state
};

static Slot g_slots[RING][INSTANCES];

// Find (or allocate) the slot for (frame, inst). Returns nullptr on
// instance-table overflow. Caller checks slot->frame to detect a stale
// (overwritten by a different frame) slot.
static Slot* find_slot(int frame, void* inst, bool allocate) {
    Slot* row = g_slots[((unsigned)frame) % RING];
    for (uint32_t i = 0; i < INSTANCES; ++i) {
        if (row[i].frame == frame && row[i].inst == inst) return &row[i];
    }
    if (!allocate) return nullptr;
    for (uint32_t i = 0; i < INSTANCES; ++i) {
        // Free or stale (different frame): reuse.
        if (row[i].inst == nullptr || row[i].frame != frame) {
            row[i].frame = frame;
            row[i].inst  = inst;
            return &row[i];
        }
    }
    return nullptr;
}

// InputCommand::Update signature: __userpurge(this=ecx) + 1 stack arg
// (retn 4). Hook signature must include the stack arg to keep the
// trampoline's retn 4 balanced — same pattern as actor2d_log's
// StepMovement hook. Reads/writes the input state at *(InputCommand+0)+4,
// which is what vtable[4]'s `return this+4` accessor exposes.
static unsigned thiscall hook(int* this_ptr, int stack_arg) {
    int f  = gekko_bridge::g_trace_frame;
    int rb = gekko_bridge::g_trace_rb;

    if (f < 0 || !this_ptr) {
        return g_h.unsafe_thiscall<unsigned>(this_ptr, stack_arg);
    }

    // *(this_ptr + 0) is the input-source pointer (an InputMulti or
    // InputGlobal). vtable[4] returns source+4; that's the 32-byte
    // {sx,sy,btn0..5} buffer the function body reads. We capture/restore
    // exactly that buffer at the read site so the body sees identical bits
    // forward and every re-sim.
    uint32_t src_ptr = (uint32_t)this_ptr[0];
    if (!src_ptr) {
        return g_h.unsafe_thiscall<unsigned>(this_ptr, stack_arg);
    }
    uint8_t* state = (uint8_t*)(uintptr_t)(src_ptr + 4);

    if (rb == 0) {
        // Forward: snapshot the state RIGHT BEFORE the function body reads
        // it. The capture happens at the same entry our trampoline jumps
        // to, so capture and the original's read see byte-identical state.
        Slot* s = find_slot(f, this_ptr, /*allocate=*/true);
        if (s) {
            memcpy(s->bytes, state, STATE_BYTES);
        } else {
            static uint32_t overflow_log = 0;
            if ((overflow_log++ & 0x3F) == 0) {
                log_printf("[igsync] ring overflow f=%d ic=%p — "
                           "more than %u InputCommands live?\n",
                           f, this_ptr, INSTANCES);
            }
        }
        return g_h.unsafe_thiscall<unsigned>(this_ptr, stack_arg);
    }

    // Re-sim: write the forward-captured bits into source+4 so the
    // function body reads identical state. Then call the original
    // normally — its writes to the InputCommand's own rings are
    // deterministic given identical inputs.
    Slot* s = find_slot(f, this_ptr, /*allocate=*/false);
    if (s) {
        memcpy(state, s->bytes, STATE_BYTES);
    } else {
        static uint32_t miss_log = 0;
        if ((miss_log++ & 0x3F) == 0) {
            log_printf("[igsync] re-sim miss f=%d ic=%p — falling through "
                       "(divergence possible)\n", f, this_ptr);
        }
    }
    return g_h.unsafe_thiscall<unsigned>(this_ptr, stack_arg);
}

} // namespace

void install() {
    // Initialise the ring to empty.
    for (uint32_t r = 0; r < RING; ++r)
        for (uint32_t i = 0; i < INSTANCES; ++i) {
            g_slots[r][i].frame = -1;
            g_slots[r][i].inst  = nullptr;
        }

    g_h = safetyhook::create_inline((void*)INPUTCOMMAND_UPDATE, (void*)hook);
    log_printf("[igsync] hook Manbow::InputCommand::Update @ 0x%X %s\n",
               (uint32_t)INPUTCOMMAND_UPDATE, g_h.enabled() ? "OK" : "FAIL");
}

} // namespace input_global_sync
