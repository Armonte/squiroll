// safetyhook MUST be included before any squiroll header — util.h #defines
// the calling-convention keywords as attribute macros and safetyhook uses
// those identifiers as method names.
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "patch_utils.h"   // _R address literal
#include "util.h"          // thiscall
#include "crash_handler.h" // watchpoint_arm — hunt the InputGlobal+4 writer
#include "log.h"
#include "gekko_bridge.h"  // dual_input_owned — stand down during dual netplay

// gekko_bridge.cpp publishes these (sq_trace pattern).
namespace gekko_bridge { extern int g_trace_frame; extern int g_trace_rb; extern int g_trace_depth; }

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
#define INPUTMULTI_UPDATE   (0x6F360_R)
#define GROUPED_INSERT      (0x11860_R)   // boost::signals2 grouped_list::insert_with_group
#define GROUPED_INSERT2     (0x30390_R)   // the Squirrel-bound per-frame connect path

namespace {

static SafetyHookInline g_h{};
static SafetyHookInline g_h_multi{};
static SafetyHookInline g_h_grp{};
static SafetyHookInline g_h_grp2{};

// 0x30390(this=signal, a2, Block, a4, a5): the per-frame Squirrel-driven draw
// connect. The group is computed inside, so just log the CONNECT SEQUENCE
// (which signal, in what order) per frame — comparing fwd rb=0 vs re-sim rb=1
// shows whether two connects to the same signal swap order (the residual).
static int thiscall grp_hook2(int this_ecx, int a2, void* Block, int a4, int a5) {
    int f  = gekko_bridge::g_trace_frame;
    int rb = gekko_bridge::g_trace_rb;
    // Log render-time (f==-1) connects too, rate-limited — confirms these draw
    // connections are made OUTSIDE the sim advance (window_render), explaining
    // why no advance-time hook caught them.
    static int n = 0;
    if (f <= 14 && n < 80) { ++n; log_printf("[grpconn] f=%d rb=%d sig=%08X\n", f, rb, (unsigned)this_ecx); }
    return g_h_grp2.unsafe_thiscall<int>(this_ecx, a2, Block, a4, a5);
}

// DIAGNOSTIC: the residual rollback divergence is a boost::signals2 grouped-list
// ORDER swap (two connections ordered differently fwd vs re-sim) -> the
// DrawCommandSlot connection head points to a different node. The order is by
// group key. If the key is a pointer-derived/transient value it diverges while
// the arena content hash stays equal (the observed symptom). Log the group key
// at the onset frames, fwd vs re-sim, to confirm + capture the diverging value.
// __thiscall(this, a2, Block, a4=groupkey*, a5, a6); `thiscall` macro = this in ecx.
static int thiscall grp_hook(int this_ecx, int a2, void* Block,
                             int* a4, int a5, int a6) {
    int f  = gekko_bridge::g_trace_frame;
    int rb = gekko_bridge::g_trace_rb;
    if (f >= 8 && f <= 11 && a4) {
        log_printf("[grpkey] f=%d rb=%d sig=%08X keytype=%08X keyval=%08X\n",
                   f, rb, (unsigned)this_ecx, (unsigned)a4[0], (unsigned)a4[2]);
    }
    return g_h_grp.unsafe_thiscall<int>(this_ecx, a2, Block, a4, a5, a6);
}

// The decoded InputGlobal/InputMulti body is x/y/b0..b11/s0..s9 (24 ints =
// 0x60) plus state — far more than the 8 ints InputCommand itself reads.
// Capturing only 32 bytes left b6..b11 + the analog sticks (offset >=0x24)
// non-deterministic; a spawn/special-move script that reads those diverges
// (two actors spawn/link in swapped order — the post-input-fix residual). The
// InputGlobal body is 0x128 bytes, so 0x80 is safely inside it and covers
// every decoded input field with headroom.
static constexpr uint32_t STATE_BYTES = 0x80;

// Per-frame, per-instance ring. Gekko rollback window = 8 frames; 16 frames
// gives headroom. We see exactly 2 InputCommand instances live per frame
// (one per player); 8 buys headroom for any future variant.
static constexpr uint32_t RING        = 16;
// Now TWO hooks share this ring (InputCommand::Update + InputMulti::Update),
// and boot frames touch many input objects. The 8-slot ring overflowed at f=0
// and dropped captures (re-sim misses on a player's instance at f=2/10/12 ->
// that input wasn't replayed -> residual cpp divergence). 64 slots/frame is
// ample headroom for both hooks across all live input objects.
static constexpr uint32_t INSTANCES   = 64;

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

    // DUAL netplay: the recorder queue + reader rebind already make the input
    // decode deterministic across re-sims (from the CORRECTED input). This
    // capture-replay would instead restore the forward-captured PREDICTED
    // (mispredicted) InputGlobal over the correct re-sim decode -> the
    // one-counter sustained desync. Stand down entirely.
    if (gekko_bridge::dual_input_owned()) {
        return g_h.unsafe_thiscall<unsigned>(this_ptr, stack_arg);
    }

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
        Slot* s = find_slot(f, this_ptr, /*allocate=*/true);
        if (s) {
            memcpy(s->bytes, state, STATE_BYTES);
        } else {
            static uint32_t overflow_log = 0;
            if ((overflow_log++ & 0x3F) == 0) {
                log_printf("[igsync] ring overflow f=%d ic=%p\n", f, this_ptr);
            }
        }
        return g_h.unsafe_thiscall<unsigned>(this_ptr, stack_arg);
    }

    Slot* s = find_slot(f, this_ptr, /*allocate=*/false);
    if (s) {
        memcpy(state, s->bytes, STATE_BYTES);
    } else {
        static uint32_t miss_log = 0;
        if ((miss_log++ & 0x3F) == 0) {
            log_printf("[igsync] re-sim miss f=%d ic=%p\n", f, this_ptr);
        }
    }
    return g_h.unsafe_thiscall<unsigned>(this_ptr, stack_arg);
}

// Manbow::InputMulti::Update (0x6F360): walks its child inputs, then invokes
// the std::function at this+0xCC (the device poll) with &(this+4), writing the
// decoded 8-int input buffer at this+4. THAT buffer is what every consumer
// reads — InputCommand AND the character state script (this.input.x in
// player_input.nut, which chooses Stand vs MoveBack). The poll is
// non-deterministic across the rollback window, so capture it on the forward
// pass and restore it on every re-sim right after the original runs — making
// the decoded input identical for ALL readers. This is the source-level fix
// the InputCommand-only read-site hook (above) missed: the state machine reads
// InputGlobal directly, bypassing InputCommand::Update. Keyed by (frame, this);
// `this` is the input buffer object, distinct from the InputCommand key.
static int thiscall hook_multi(int* this_ptr) {
    int r = g_h_multi.unsafe_thiscall<int>(this_ptr);   // poll + write this+4..36
    int f  = gekko_bridge::g_trace_frame;
    int rb = gekko_bridge::g_trace_rb;
    // DUAL netplay: stand down (see hook() above — same clobber mechanism).
    if (gekko_bridge::dual_input_owned())
        return r;
    if (f >= 0 && this_ptr) {
        uint8_t* state = (uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)this_ptr + 4);
        if (rb == 0) {
            Slot* s = find_slot(f, this_ptr, /*allocate=*/true);
            if (s) memcpy(s->bytes, state, STATE_BYTES);
        } else {
            Slot* s = find_slot(f, this_ptr, /*allocate=*/false);
            if (s) memcpy(state, s->bytes, STATE_BYTES);
        }
        // DEPTH PROBE: dump the WHOLE InputMulti (0x128) at f=15 with rollback
        // depth -- find the dword that diverges depth-1(divergent) vs depth-0
        // (forward). The capture covers +4..+0x84; +0 and the +0x84..+0x128 tail
        // are NOT captured -> if X (the un-saved carry-over the move reads) is in
        // InputMulti, it's there. Logged AFTER capture/replay (so +4..+0x84 is
        // identical; any diff is in +0 or the tail).
        if (f == 15) {
            uint32_t* o = (uint32_t*)(uintptr_t)this_ptr;
            for (uint32_t b = 0; b < 0x128; b += 0x20) {
                log_printf("[imdump] f=15 rb=%d d=%d ip=%08X +%03X: "
                           "%08X %08X %08X %08X %08X %08X %08X %08X\n",
                           rb, gekko_bridge::g_trace_depth, (uint32_t)(uintptr_t)this_ptr, b,
                           o[b/4+0], o[b/4+1], o[b/4+2], o[b/4+3],
                           o[b/4+4], o[b/4+5], o[b/4+6], o[b/4+7]);
            }
        }
    }
    return r;
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

    g_h_multi = safetyhook::create_inline((void*)INPUTMULTI_UPDATE, (void*)hook_multi);
    log_printf("[igsync] hook Manbow::InputMulti::Update @ 0x%X %s (source-level "
               "input determinism for ALL readers)\n",
               (uint32_t)INPUTMULTI_UPDATE, g_h_multi.enabled() ? "OK" : "FAIL");

    g_h_grp = safetyhook::create_inline((void*)GROUPED_INSERT, (void*)grp_hook);
    log_printf("[grpkey] hook grouped_list::insert_with_group @ 0x%X %s\n",
               (uint32_t)GROUPED_INSERT, g_h_grp.enabled() ? "OK" : "FAIL");

    g_h_grp2 = safetyhook::create_inline((void*)GROUPED_INSERT2, (void*)grp_hook2);
    log_printf("[grpconn] hook squirrel connect path @ 0x%X %s\n",
               (uint32_t)GROUPED_INSERT2, g_h_grp2.enabled() ? "OK" : "FAIL");
}

} // namespace input_global_sync
