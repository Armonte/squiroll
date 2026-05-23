// safetyhook MUST be included before any squiroll header — util.h #defines
// the calling-convention keywords as attribute macros and safetyhook uses
// those identifiers as method names.
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <intrin.h>

#include "patch_utils.h"   // _R address literal
#include "util.h"          // thiscall, base_address
#include "log.h"

namespace gekko_bridge { extern int g_trace_frame; extern int g_trace_rb; }

namespace input_command_log {

// Manbow::InputCommand::Update at 0x74390. The hot loop reads input via
// (*this+0)->vtable[4](*this) -> a pointer to a {stick_x, stick_y, btn0..5}
// int array, packs the 8 fields into combined bits, and stores them in two
// 120-entry uint16 rings on the InputCommand. Battle_pools diff_locate
// found this ring (offset 0x74+) diverges between forward and EVERY re-sim
// from frame=2 onwards — the input source is reading non-deterministic
// (un-synced) state.
//
// Goal: identify the input source class by logging its vtable pointer and
// the resulting input bits. The vtable ptr → IDA name pins which TF4 input
// device implementation is bypassing gekko's forced_inputs override.
#define INPUTCOMMAND_UPDATE (0x74390_R)

// Same diag window as the others — but widen to f=2..10 since the
// divergence starts at f=2 (not at f=15 like UCM). retn 4 — see comments.
static constexpr int F_LO = 1;
static constexpr int F_HI = 6;

namespace {

static SafetyHookInline g_h{};

// InputCommand::Update signature from IDA: __thiscall(this, arg). Function
// ends with `retn 4`, so the stack arg must be re-pushed when calling the
// trampoline (same pattern as actor2d_log's StepMovement hook).
static unsigned thiscall hook(int* this_ptr, int arg) {
    int f  = gekko_bridge::g_trace_frame;
    int rb = gekko_bridge::g_trace_rb;

    if (this_ptr && f >= F_LO && f <= F_HI) {
        uint32_t src_ptr  = (uint32_t)this_ptr[0];
        uint32_t src_vtbl = src_ptr ? *(uint32_t*)src_ptr : 0;
        uint32_t vt_pollfn = (src_vtbl != 0)
            ? *(uint32_t*)(src_vtbl + 16)   // vtable[4]
            : 0;
        uint32_t ring_idx = *((uint8_t*)this_ptr + 0x204);
        uint32_t caller_rva =
            (uint32_t)((uintptr_t)_ReturnAddress() - base_address);

        log_printf("[icmd] f=%d rb=%d by=%05X this=%08X src=%08X "
                   "src_vt=%08X poll_rva=%05X ring_idx=%u arg=%d\n",
                   f, rb, caller_rva, (uint32_t)(uintptr_t)this_ptr,
                   src_ptr, src_vtbl,
                   vt_pollfn ? (vt_pollfn - (uint32_t)base_address) : 0,
                   ring_idx, arg);
    }

    return g_h.unsafe_thiscall<unsigned>(this_ptr, arg);
}

} // namespace

void install() {
    g_h = safetyhook::create_inline((void*)INPUTCOMMAND_UPDATE, (void*)hook);
    log_printf("[icmd] hook Manbow::InputCommand::Update @ 0x%X %s\n",
               (uint32_t)INPUTCOMMAND_UPDATE,
               g_h.enabled() ? "OK" : "FAIL");
}

} // namespace input_command_log
