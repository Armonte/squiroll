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
#include "sq_arena.h"      // base() — range-check the closure->name decode

namespace gekko_bridge { extern int g_trace_frame; extern int g_trace_rb; extern int g_trace_depth; }

namespace sqfun_log {

// Sqrat::Function::Execute_1_param at 0xB860.
//
// The function is __thiscall(Sqrat::Function*). The Sqrat::Function instance
// layout, reconstructed from the disassembly at 0xB88A..0xB8A1:
//   *(this + 0x00) = HSQUIRRELVM*  (the VM the function executes on)
//   *(this + 0x04) = SQObjectType  function (closure) type
//   *(this + 0x08) = SQObjectValue function (closure) value — pointer to
//                                  the SQClosure / SQNativeClosure
//   *(this + 0x0C) = SQObjectType  env (the captured 'this') type
//   *(this + 0x10) = SQObjectValue env value — usually a SQTable*
//
// Update_mask1 fires this between its Update loop and its StepMovement loop
// (call site at RVA 0x9B3DF — i.e. _ReturnAddress() reads ~0x9B3E4 in our
// log). If the per-actor velocity diverges between forward and a re-sim of
// the SAME frame, this callback is the prime suspect — it's the only thing
// that runs Squirrel between load and the StepMovement read of vx/vy.
#define SQRAT_FUNCTION_EXECUTE_1_PARAM (0xB860_R)

// Diagnostic frame window — keep this narrow so the per-call dump doesn't
// drown the log. Same range we use in actor2d_log.
static constexpr int F_LO = 13;
static constexpr int F_HI = 16;

namespace {

static SafetyHookInline g_h{};

static int thiscall hook(int* sqrat_fn) {
    int f  = gekko_bridge::g_trace_frame;
    int rb = gekko_bridge::g_trace_rb;

    if (f >= F_LO && f <= F_HI && sqrat_fn) {
        uint32_t caller_rva =
            (uint32_t)((uintptr_t)_ReturnAddress() - base_address);

        uint32_t vm     = (uint32_t)sqrat_fn[0];
        uint32_t f_type = (uint32_t)sqrat_fn[1];
        uint32_t f_val  = (uint32_t)sqrat_fn[2];
        uint32_t e_type = (uint32_t)sqrat_fn[3];
        uint32_t e_val  = (uint32_t)sqrat_fn[4];

        // For SQObjectType = OT_CLOSURE (0x08000000 in Squirrel 3.x), f_val
        // is a SQClosure*. We don't decode it here — the divergence we
        // care about is whether (f_type, f_val) is the SAME across all 8
        // advance(15) attempts. If it is, the function identity is stable
        // and the divergence is INSIDE the script's body (it reads some
        // non-captured state). If not, the Sqrat::Function field on the
        // Actor2DGroup at +0x74 (=this+29 in the IDA decompile) is the
        // divergent state.
        // Decode the closure NAME: SQClosure[8] = _function (SQFunctionProto),
        // funcproto[9] = _name (SQString*), chars at +0x1C. Shows WHICH .nut
        // function runs -> the divergent f=15 call (MoveBack vs Stand etc).
        char nm[48]; nm[0] = '?'; nm[1] = 0;
        uint32_t lo = (uint32_t)(uintptr_t)sq_arena::base();
        uint32_t hi = lo + 0x4000000u;   // 64MB window — closures live in here
        if (f_val >= lo && f_val + 0x24 < hi) {
            uint32_t fp = ((uint32_t*)(uintptr_t)f_val)[8];       // _function
            if (fp >= lo && fp + 0x28 < hi) {
                uint32_t sname = ((uint32_t*)(uintptr_t)fp)[9];   // _name SQString*
                if (sname >= lo && sname + 0x40 < hi) {
                    const char* s = (const char*)(uintptr_t)(sname + 0x1C);
                    int i = 0; for (; i < 46 && s[i]; ++i) nm[i] = s[i]; nm[i] = 0;
                }
            }
        }
        log_printf("[sqfun] f=%d rb=%d d=%d by=%05X ft=%08X name='%s' fv=%08X ev=%08X\n",
                   f, rb, gekko_bridge::g_trace_depth, caller_rva, f_type, nm, f_val, e_val);
    }

    return g_h.unsafe_thiscall<int>(sqrat_fn);
}

} // namespace

void install() {
    g_h = safetyhook::create_inline((void*)SQRAT_FUNCTION_EXECUTE_1_PARAM,
                                    (void*)hook);
    log_printf("[sqfun] hook Sqrat::Function::Execute_1_param @ 0x%X %s\n",
               (uint32_t)SQRAT_FUNCTION_EXECUTE_1_PARAM,
               g_h.enabled() ? "OK" : "FAIL");
}

} // namespace sqfun_log
