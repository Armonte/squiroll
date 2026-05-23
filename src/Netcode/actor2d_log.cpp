// safetyhook MUST be included before any squiroll header — util.h #defines
// the calling-convention keywords as attribute macros and safetyhook uses
// those identifiers as method names. (Same ordering as cpp_arena.cpp.)
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>

#include "patch_utils.h"   // _R address literal
#include "util.h"          // thiscall
#include "log.h"

// gekko_bridge.cpp publishes these; same `extern int` pattern sq_trace uses.
//   g_trace_frame: the gekko frame currently being advanced (-1 outside an
//                  advance, e.g. when the engine is updating the menu).
//   g_trace_rb:    non-zero when the current advance is a rollback re-sim.
namespace gekko_bridge { extern int g_trace_frame; extern int g_trace_rb; }

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

namespace {

static SafetyHookInline g_h{};

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

        log_printf("[a2dlog] f=%d rb=%d this=%08X ct=%u cc=%u %s "
                   "vb=%08X ve=%08X cb=%08X ce=%08X g56=%08X g112=%02X\n",
                   f, rb, (uint32_t)this_ptr,
                   count_this, count_child, branch,
                   v_begin, v_end, c_begin, c_end,
                   guard_bp, (unsigned)guard_flag);
    }

    return g_h.unsafe_thiscall<int>(this_ptr);
}

} // namespace

void install() {
    g_h = safetyhook::create_inline((void*)ACTOR2D_UPDATE_CHILD_MATRICES,
                                    (void*)hook);
    log_printf("[a2dlog] hook Actor2D::UpdateChildMatrices @ 0x%X %s\n",
               (uint32_t)ACTOR2D_UPDATE_CHILD_MATRICES,
               g_h.enabled() ? "OK" : "FAIL");
}

} // namespace actor2d_log
