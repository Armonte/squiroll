// safetyhook MUST be included before any squiroll header — util.h #defines
// the calling-convention keywords as attribute macros and safetyhook uses
// those identifiers as method names. (Same ordering as cpp_arena.cpp.)
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <intrin.h>        // _ReturnAddress

#include "patch_utils.h"   // _R address literal, base_address
#include "util.h"          // thiscall
#include "log.h"

// gekko_bridge.cpp publishes these; same `extern int` pattern sq_trace uses.
//   g_trace_frame: the gekko frame currently being advanced (-1 outside an
//                  advance, e.g. when the engine is updating the menu).
//   g_trace_rb:    non-zero when the current advance is a rollback re-sim.
namespace gekko_bridge { extern int g_trace_frame; extern int g_trace_rb; extern int g_trace_depth; }

namespace actor2d_log {

// Manbow::Actor2D::UpdateChildMatrices at 0xC2B40 — the function whose
// count_this(this+156..160) vs count_child(*(this+60)+144..148) comparison at
// 0xC2C29 (`cmp eax, ecx; ja ...`) selects between the setAabb path (THEN)
// and the createProxy-with-(-1) path (ELSE). The forward run hits createProxy
// at f=15; re-sims only hit setAabb. The state going into f=15 is byte-
// identical ([comp] sq+bt+cpp+sblob matches at f=14), so something
// un-captured produces the asymmetry. To pin which Actor2D's counts diverge,
// log them every call inside the diagnostic frame window.
//
// Prologue is 17 bytes before any branch (push ebp; mov ebp,esp;
// and esp,F0; sub esp,128; mov eax,___security_cookie) — well over the 5
// safetyhook needs for the JMP trampoline.
#define ACTOR2D_UPDATE_CHILD_MATRICES (0xC2B40_R)

// Manbow::Actor2D::StepMovement at 0xC1330 — the caller whose presence
// distinguishes the divergent re-sim from the matching forward+re-sim
// runs. StepMovement reaches UpdateChildMatrices only when BOTH:
//   (a) *(actor+228) is null, OR **(actor+228) is 0 (the "no task vector
//       blocking the move" gate), AND
//   (b) at least one of xmm2/xmm3 (loaded from actor[0x2C]/[0x30] by the
//       Update_mask0 call site at 0x9B308) is non-zero (i.e. there IS
//       movement).
// We log these inputs every call so the frame-15 forward-vs-resim asymmetry
// can be attributed to whichever of (a)/(b) flips between runs.
#define ACTOR2D_STEP_MOVEMENT          (0xC1330_R)

namespace {

static SafetyHookInline g_h{};
static SafetyHookInline g_h_step{};

// Diagnostic window — keep narrow so the per-Actor2D log doesn't spam the
// whole match. Forward advance(15) + every re-sim that hits 15 print one
// line per Actor2D; the first row where (count_this, count_child) differs
// forward vs re-sim pins the producer.
static constexpr int F_LO = 13;
static constexpr int F_HI = 16;

// Manbow::Actor2D layout (offsets verified against IDA decompile @ 0xC2B40):
//   this+0x3C (60) = child object pointer (the "+60" referent the function
//                    repeatedly derefs). Its +144/+148 are vector_child
//                    {begin, end} (record size 8).
//   this+0x6C (108) = some container pointer (vtable+32 / vtable+36 dispatch
//                     source — irrelevant to the branch decision).
//   this+0x9C (156) = vector_this.begin (record size 8).
//   this+0xA0 (160) = vector_this.end.
//
// Branch at 0xC2C29:
//   count_this  > count_child  -> THEN (setAabb only, vtable+36, 2 args)
//   count_this <= count_child  -> ELSE (createProxy w/-1 sentinel, vtable+32,
//                                       3 args — this is the path the
//                                       forward run hits at f=15)
static int thiscall hook(int this_ptr) {
    int f  = gekko_bridge::g_trace_frame;
    int rb = gekko_bridge::g_trace_rb;

    // Caller RVA so we can attribute each call to its xref. The known
    // callers all live in th155 (this is a th155-internal function):
    //   0xC133D StepMovement (Update_mask0/1 inner loop)
    //   0xC13F0 sub_C13F0    (Update_mask2 inner loop)
    //   0xC1520 sub_C1520    (Actor2D::InvokeFuncImpl)
    //   0xC15D0 Actor2D::Warp
    //   0xC1610 Actor2D::Warp3D
    //   0x95D22 Actor2D::SetParent
    //   0xC1206 Actor2DManager::InitActorState
    //   0xC1D00 a std::function lambda
    // Knowing which entry path fires at the divergent frame separates a
    // per-frame StepMovement (mask0/1 vs mask2) divergence from a
    // Warp/SetParent triggered by a Squirrel script callback in between.
    uint32_t caller_rva =
        (uint32_t)((uintptr_t)_ReturnAddress() - base_address);

    if (f >= F_LO && f <= F_HI) {
        uint32_t v_begin = *(uint32_t*)(this_ptr + 156);
        uint32_t v_end   = *(uint32_t*)(this_ptr + 160);
        uint32_t count_this = (v_end - v_begin) >> 3;

        uint32_t child       = *(uint32_t*)(this_ptr + 60);
        uint32_t c_begin     = 0;
        uint32_t c_end       = 0;
        uint32_t count_child = 0;
        if (child) {
            c_begin = *(uint32_t*)(child + 144);
            c_end   = *(uint32_t*)(child + 148);
            count_child = (c_end - c_begin) >> 3;
        }

        // The 0xC2BF5/0xC2BFF early-out: jz to loc_C2DC0 when either
        //   *(this+108)+56 == 0   OR   (*(this+112) & 1) == 0
        // We log this too so we can see whether forward/re-sim agree on
        // taking the active-broadphase path at all.
        uint32_t guard108 = *(uint32_t*)(this_ptr + 108);
        uint32_t guard_bp = guard108 ? *(uint32_t*)(guard108 + 56) : 0;
        uint8_t  guard_flag = *(uint8_t*)(this_ptr + 112);
        bool     active = (guard_bp != 0) && ((guard_flag & 1) != 0);

        const char* branch;
        if (!active)                         branch = "skip";
        else if (count_this > count_child)   branch = "THEN(setAabb)";
        else                                  branch = "ELSE(createProxy)";

        log_printf("[a2dlog] f=%d rb=%d this=%08X by=%05X ct=%u cc=%u %s "
                   "vb=%08X ve=%08X cb=%08X ce=%08X g56=%08X g112=%02X\n",
                   f, rb, (uint32_t)this_ptr, caller_rva,
                   count_this, count_child, branch,
                   v_begin, v_end, c_begin, c_end,
                   guard_bp, (unsigned)guard_flag);
    }

    return g_h.unsafe_thiscall<int>(this_ptr);
}

// StepMovement entry hook. The function's calling convention as IDA
// reconstructs it is __userpurge: ecx=this, xmm2=dx, xmm3=dy AND a 4-byte
// stack arg (the function ends `retn 4`). The Update_mask0 call site
// pushes ecx (the actor again) as that stack arg before the call. To keep
// the original's `retn 4` cleanup balanced, the hook MUST also receive
// that stack arg and pass it through to the trampoline. Declaring the
// hook as thiscall(actor, stack_arg) puts actor in ecx and stack_arg on
// the stack; the trampoline call re-pushes stack_arg, the original's
// retn 4 pops it, balanced.
//
// IMPORTANT: do NOT do float arithmetic in this hook — xmm2/xmm3 contain
// the caller's velocity arguments, and GCC will clobber them if asked to
// allocate xmm registers. Read the velocity fields back as uint32 (bit
// pattern) and log them as hex; the consumer recovers floats by reading
// them as IEEE-754.
static int thiscall hook_step(int actor, int stack_arg) {
    int f  = gekko_bridge::g_trace_frame;
    int rb = gekko_bridge::g_trace_rb;

    if (f >= F_LO && f <= F_HI) {
        // gate (a): actor+228 — the "task vector head" pointer. If non-null
        // AND its first dword is non-zero, StepMovement returns early and
        // never reaches UpdateChildMatrices.
        uint32_t task_head = *(uint32_t*)(actor + 228);
        uint32_t task_head_0 = task_head ? *(uint32_t*)task_head : 0;
        bool gate_a_passes = (task_head == 0) || (task_head_0 == 0);

        // gate (b): movement deltas. Read the bit patterns at the exact
        // fields the caller sources xmm2/xmm3 from in Update_mask0 at
        // 0x9B308/0x9B30D. We compare against 0 as a uint32 — IEEE-754
        // +0.0 is 0x00000000; -0.0 is 0x80000000 and would still pass the
        // (xmm != 0) check, but we don't need exact float semantics —
        // any non-zero bit pattern indicates non-zero velocity in
        // practice.
        uint32_t vx_bits = *(uint32_t*)(actor + 0x2C);
        uint32_t vy_bits = *(uint32_t*)(actor + 0x30);
        bool gate_b_passes = (vx_bits != 0) || (vy_bits != 0);

        const char* ucm_branch =
            gate_a_passes
                ? (gate_b_passes ? "UCM"            // movement path
                                 : "noUCM(v=0)")     // no-movement early-out
                : "noUCM(task)";                     // task vector blocks move

        uint32_t caller_rva =
            (uint32_t)((uintptr_t)_ReturnAddress() - base_address);

        log_printf("[a2dstep] f=%d rb=%d d=%d this=%08X by=%05X "
                   "th=%08X th0=%08X vx=%08X vy=%08X %s\n",
                   f, rb, gekko_bridge::g_trace_depth, (uint32_t)actor, caller_rva,
                   task_head, task_head_0, vx_bits, vy_bits, ucm_branch);
        // Dump the actor position/state region [0x18..0x40] so the FIRST
        // divergent actor field (fwd vs deepest re-sim) at f=14/15 shows up --
        // the value the move-selection read (position fed to GetFront, etc).
        if (f >= 14 && f <= 15) {
            log_printf("[a2dfld] f=%d rb=%d d=%d this=%08X "
                       "18=%08X 1C=%08X 20=%08X 24=%08X 28=%08X 2C=%08X "
                       "30=%08X 34=%08X 38=%08X 3C=%08X\n",
                       f, rb, gekko_bridge::g_trace_depth, (uint32_t)actor,
                       *(uint32_t*)(actor + 0x18), *(uint32_t*)(actor + 0x1C),
                       *(uint32_t*)(actor + 0x20), *(uint32_t*)(actor + 0x24),
                       *(uint32_t*)(actor + 0x28), *(uint32_t*)(actor + 0x2C),
                       *(uint32_t*)(actor + 0x30), *(uint32_t*)(actor + 0x34),
                       *(uint32_t*)(actor + 0x38), *(uint32_t*)(actor + 0x3C));
        }
    }

    return g_h_step.unsafe_thiscall<int>(actor, stack_arg);
}

} // namespace

void install() {
    g_h = safetyhook::create_inline((void*)ACTOR2D_UPDATE_CHILD_MATRICES,
                                    (void*)hook);
    log_printf("[a2dlog] hook Actor2D::UpdateChildMatrices @ 0x%X %s\n",
               (uint32_t)ACTOR2D_UPDATE_CHILD_MATRICES,
               g_h.enabled() ? "OK" : "FAIL");

    g_h_step = safetyhook::create_inline((void*)ACTOR2D_STEP_MOVEMENT,
                                         (void*)hook_step);
    log_printf("[a2dlog] hook Actor2D::StepMovement @ 0x%X %s\n",
               (uint32_t)ACTOR2D_STEP_MOVEMENT,
               g_h_step.enabled() ? "OK" : "FAIL");
}

} // namespace actor2d_log
