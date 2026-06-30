// safetyhook MUST be included before any squiroll header — util.h #defines
// the calling-convention keywords as attribute macros and safetyhook uses
// those identifiers as method names. (Same ordering as cpp_arena.cpp.)
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <intrin.h>        // _ReturnAddress

#include <stdlib.h>        // malloc/free for the arming-thread request
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
// --- velocity-writer watchpoint (DR0) -------------------------------------
// X-NAMING: the f=15 velocity (actor+0x2C) is 0 at f=14 (all depths) and 11.5
// only in the d=1 re-sim. We arm a Dr0 write-watch on the moving actor's +0x2C
// at f=14 (before the f=15 divergence) and a VEH logs only NON-ZERO writes
// (skips Stand's 0-writes so the hit budget catches the 11.5 write) with the
// th155 stack return-chain -> the .nut/native path that read the un-saved X and
// produced 11.5. Dr0 is per-thread; arming on the sim thread catches the
// sim-thread script write. Confirmed free (battle_pools' user is dormant).
static void*    g_vw_veh  = nullptr;
static uint32_t g_vw_addr = 0;
static int      g_vw_hits = 0;

static LONG CALLBACK vw_veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    uint32_t now = *(const uint32_t*)(uintptr_t)g_vw_addr;
    // Repurposed for the game-loop slot-list use_count trace: log every write whose
    // RESULT is small (<=2) — captures the lock(->2)/release(->1) and the critical drop
    // to 0 — with the writer rva + frame + rb. This shows EXACTLY who decrements the
    // use_count to 0 and on which side (forward vs rollback re-sim).
    if (now != 0 && g_vw_hits < 400) {
        ++g_vw_hits;
        log_printf("[velwatch] %08X <- val=%08X rva=%08X f=%d rb=%d d=%d\n",
                   g_vw_addr, now, (uint32_t)(c->Eip - base_address),
                   gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb,
                   gekko_bridge::g_trace_depth);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct VwArm { HANDLE thread; uint32_t addr; };
static unsigned long __stdcall vw_arm_thread(void* p) {
    VwArm* r = (VwArm*)p;
    SuspendThread(r->thread);
    CONTEXT c; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(r->thread, &c)) {
        c.Dr0 = r->addr;
        // Dr0 enabled (bit0); LEN=01 (2-byte, bits16-17); R/W=11 (read+write,
        // bits18-19). 2-byte read/write is the config that works reliably here
        // (4-byte/write-only hung the game). The 2-byte window at va.x's low
        // half still catches every float write; the VEH logs the current value.
        c.Dr7 = (c.Dr7 & ~0xF0001u) | 1u | (1u << 16) | (3u << 18);
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        SetThreadContext(r->thread, &c);
    }
    ResumeThread(r->thread); CloseHandle(r->thread); free(r);
    return 0;
}

static void vw_arm(uint32_t addr) {
    if (g_vw_addr) return;  // arm once
    g_vw_addr = addr;
    g_vw_veh = AddVectoredExceptionHandler(1, vw_veh);
    HANDLE self = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
    VwArm* r = (VwArm*)malloc(sizeof(VwArm));
    r->thread = self; r->addr = addr;
    CloseHandle(CreateThread(nullptr, 0, vw_arm_thread, r, 0, nullptr));
    log_printf("[velwatch] armed Dr0 write-watch on %08X (actor+0x2C)\n", addr);
}

static int thiscall hook_step(int actor, int stack_arg) {
    int f  = gekko_bridge::g_trace_frame;
    int rb = gekko_bridge::g_trace_rb;

    // Arm the velocity watch on the FIRST actor seen at f=14 (the player at the
    // lowest pool address = the confirmed 11.5 mover). Before f=15's write.
    if (f == 14 && !g_vw_addr && actor)
        vw_arm((uint32_t)actor + 0x2C);

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

// --- velocity-SET closure-stack walk (names the .nut walk function = X) -----
// Sqrat__SetMemberVar_float @ 0x46A50 is the generic float member SETTER that
// writes this.va.x (actor velocity +0x2C). [velwatch] proved the f=15 d=1 write
// of 11.5 comes from here, but the setter only STORES its arg -- the walk-vs-
// stand decision is UPSTREAM in the .nut. When the setter writes the WATCHED
// actor's +0x2C (== g_vw_addr, armed by velwatch at f=14), we walk the live
// SQVM CallInfo stack and log every .nut closure frame by name. Comparing the
// d=1 chain (writes 11.5 -> walks) against the d=2/forward chain (writes
// brake/0 -> stands) at f=15 names the exact .nut function whose branch
// diverged -> that function's source reveals the un-saved engine value (X) it
// read. Same closure-name walk the working [objid] dump uses.
//
// SQVM layout (from SQVM__CallNative @ 0x18DA80 / SQVM__EnterFrame @ 0x18E530):
//   VM+0x60 (dword[24]) = _callsstack base
//   VM+0x64 (dword[25]) = _callsstacksize (frame count)
//   CallInfo stride     = 44 (0x2C) bytes; top frame (_ci) = base+44*(size-1)
//   ci+0x08 = _closure._type   ci+0x0C = _closure._unVal (the closure object)
//   OT_CLOSURE = 0x08000100 (.nut), OT_NATIVECLOSURE = 0x08000200 (C++)
// SQClosure: +0x20 = _function (SQFunctionProto); FP+0x24 = _name (SQString*);
//   SQString chars at +0x1C.
#define SQRAT_SET_MEMBER_VAR_FLOAT (0x46A50_R)

// Clean cdecl SQ stack readers (default x86 conv == cdecl). We mirror the exact
// reads the setter itself does so this can't disturb the VM (pure reads).
typedef int (*sq_getfloat_t)(int vm, int idx, float* out);
typedef int (*sq_getinstanceup_t)(int vm, int idx, void** out, int tag);
typedef int (*sq_getuserdata_t)(int vm, int idx, void** out, int tag);

static SafetyHookInline g_h_set{};
static int g_setwalk_budget = 2000;   // hard cap so the f=15 re-sims can't spam

// True if [a, a+n) is committed & readable. VirtualQuery-based so a stale
// pointer in the SQVM walk can't fault the sim thread (no SEH under 32-bit
// MinGW; the proven [objid] walk relies on a range check instead -- this is the
// same idea but checks the actual page state so it works outside sq_arena too).
static bool rd_ok(uint32_t a, uint32_t n) {
    if (!a || n == 0) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)(uintptr_t)a, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    uint32_t rbase = (uint32_t)(uintptr_t)mbi.BaseAddress;
    return (uint64_t)a + n <= (uint64_t)rbase + mbi.RegionSize;
}

// Copy a SQString (chars at +0x1C) into out, range-checked.
static void decode_sqstring(uint32_t sstr, char* out, int cap) {
    out[0] = '?'; out[1] = 0;
    if (!rd_ok(sstr + 0x1C, 1)) return;
    const char* s = (const char*)(uintptr_t)(sstr + 0x1C);
    int i = 0;
    for (; i < cap - 1 && rd_ok((uint32_t)(uintptr_t)(s + i), 1) && s[i]; ++i)
        out[i] = s[i];
    out[i] = 0;
}

// Decode a closure's _function -> name (FP+0x24), source file (FP+0x1C) and
// the function's defining line (FP+0x28 = SQFunctionProto::_lineinfos[0].line,
// the first line-info entry). The source+line locate even anonymous functions
// (no _name) -- which is exactly the VX_Brake caller (#9) that prints '?'.
static void decode_closure(uint32_t cobj, char* nm, char* src, int cap, uint32_t* line) {
    nm[0] = '?'; nm[1] = 0; src[0] = '?'; src[1] = 0; if (line) *line = 0;
    if (!rd_ok(cobj + 0x20, 4)) return;
    uint32_t fp = *(uint32_t*)(uintptr_t)(cobj + 0x20);   // _function (SQFunctionProto)
    if (rd_ok(fp + 0x24, 4)) decode_sqstring(*(uint32_t*)(uintptr_t)(fp + 0x24), nm, cap);
    if (rd_ok(fp + 0x1C, 4)) decode_sqstring(*(uint32_t*)(uintptr_t)(fp + 0x1C), src, cap);
    // _lineinfos pointer + first line. Layout (SQ 3.0.x): FP+0x28 _lineinfos*,
    // each SQLineInfo = {SQInteger _line; SQInteger _op} (8 bytes), _line@+0.
    if (line && rd_ok(fp + 0x28, 4)) {
        uint32_t li = *(uint32_t*)(uintptr_t)(fp + 0x28);
        if (rd_ok(li, 4)) *line = *(uint32_t*)(uintptr_t)li;
    }
}

// Walk every CallInfo frame from the top (_ci) down to the stack base, printing
// each closure frame's kind + .nut name. The topmost OT_CLOSURE frames are the
// .nut functions executing the member set; the chain root-to-tip IS the
// decision path (e.g. player_input -> MoveFront -> SetSpeed -> [native setter]).
static void log_nut_stack(uint32_t vm, int f, int rb, int d,
                          uint32_t tgt, uint32_t valbits) {
    if (!rd_ok(vm + 0x64, 4)) return;
    uint32_t cs_base = *(uint32_t*)(uintptr_t)(vm + 0x60);
    uint32_t cs_size = *(uint32_t*)(uintptr_t)(vm + 0x64);
    if (!cs_base || cs_size == 0 || cs_size > 256) return;
    log_printf("[setwalk] f=%d rb=%d d=%d tgt=%08X val=%08X nframes=%u\n",
               f, rb, d, tgt, valbits, cs_size);
    for (int i = (int)cs_size - 1; i >= 0; --i) {
        uint32_t c = cs_base + 44u * (uint32_t)i;
        if (!rd_ok(c + 8, 8)) continue;
        uint32_t ctype = *(uint32_t*)(uintptr_t)(c + 8);
        uint32_t cobj  = *(uint32_t*)(uintptr_t)(c + 12);
        const char* kind = (ctype == 0x08000100) ? "nut"
                         : (ctype == 0x08000200) ? "native" : "?";
        char nm[48], src[64]; uint32_t line = 0;
        if (ctype == 0x08000100) decode_closure(cobj, nm, src, sizeof(nm), &line);
        else { nm[0] = '-'; nm[1] = 0; src[0] = '-'; src[1] = 0; }
        log_printf("[setwalk]   #%-2d %-6s '%s'  @ %s:%u\n",
                   i, kind, nm, src, line);
    }
}

// __cdecl detour on the float member setter. We re-read slots 1/-1/2 exactly as
// the original does (instance up, member offset userdata, value) to recover the
// write target; if it matches the watched actor's +0x2C we dump the .nut stack.
static int cdecl hook_set(int vm) {
    int f = gekko_bridge::g_trace_frame;
    // f=24 vx divergence: log the .nut stack of EVERY vx write (member off 0x2C
    // = Actor2D vx) so the divergent-value write (8.75 vs 3.75, val=410C0000 vs
    // 40700000) is attributed to the exact .nut function setting it directly
    // (ConvertTotalSpeed's components are identical across depths, so this is a
    // direct `this.vx = ...` from some other path).
    if (f == 24 && g_setwalk_budget > 0 && vm) {
        sq_getinstanceup_t p_giu = (sq_getinstanceup_t)(0x182d60_R);
        sq_getuserdata_t   p_gud = (sq_getuserdata_t)(0x1834f0_R);
        sq_getfloat_t      p_gf  = (sq_getfloat_t)(0x182bf0_R);
        void* obj = nullptr; void* udp = nullptr; float val = 0.0f;
        p_giu(vm, 1, &obj, 0);
        p_gud(vm, -1, &udp, 0);
        p_gf(vm, 2, &val);
        uint32_t off = udp ? *(uint32_t*)udp : 0xFFFFFFFFu;
        uint32_t tgt = (uint32_t)(uintptr_t)obj + off;
        if (off == 0x2C) {   // Actor2D.vx
            --g_setwalk_budget;
            uint32_t vb; __builtin_memcpy(&vb, &val, 4);
            log_nut_stack((uint32_t)vm, f, gekko_bridge::g_trace_rb,
                          gekko_bridge::g_trace_depth, tgt, vb);
        }
    }
    return g_h_set.unsafe_ccall<int>(vm);
}

// --- component COMPARE: which of va.x/vf.x/vfBaria.x is the stale X ---------
// player_game.nut ConvertTotalSpeed() does:
//   this.vx = this.va.x + this.vf.x + this.vfBaria.x;
// i.e. the engine vx (actor+0x2C, the value [setwalk]/[velwatch] caught) is
// RECOMPUTED every frame from three Vector3() sub-objects (va, vf, vfBaria).
// Control flow at f=15 is byte-identical d=0 vs d=1, so the divergence is pure
// DATA: one of those three component reads returns a stale value in the d=1
// re-sim (which loaded save(14) and SKIPPED re-running frame 14, so a field
// frame-14 wrote but save(14) didn't capture is wrong). Sqrat__GetMemberVar_
// float @ 0x469F0 is the float GETTER ConvertTotalSpeed uses; we log each read
// (obj+offset+value) at f=15 whose immediate .nut caller is ConvertTotalSpeed.
// Group by (obj,off) and compare val across depth -> the obj whose .x differs
// d=0 vs d=1 IS the un-saved component == X.
#define SQRAT_GET_MEMBER_VAR_FLOAT (0x469F0_R)

static SafetyHookInline g_h_get{};
static int g_getcmp_budget = 4000;

// Name of the topmost OT_CLOSURE (.nut) frame -- the immediate script caller of
// the native getter.
static void topmost_nut_name(uint32_t vm, char* out, int cap) {
    out[0] = '?'; out[1] = 0;
    if (!rd_ok(vm + 0x64, 4)) return;
    uint32_t cs_base = *(uint32_t*)(uintptr_t)(vm + 0x60);
    uint32_t cs_size = *(uint32_t*)(uintptr_t)(vm + 0x64);
    if (!cs_base || cs_size == 0 || cs_size > 256) return;
    for (int i = (int)cs_size - 1; i >= 0; --i) {
        uint32_t c = cs_base + 44u * (uint32_t)i;
        if (!rd_ok(c + 8, 8)) continue;
        uint32_t ty = *(uint32_t*)(uintptr_t)(c + 8);
        uint32_t co = *(uint32_t*)(uintptr_t)(c + 12);
        if (ty == 0x08000100) { char src[64]; decode_closure(co, out, src, cap < 64 ? cap : 64, nullptr); return; }
    }
}

static int cdecl hook_get(int vm) {
    int f = gekko_bridge::g_trace_frame;
    if (f == 24 && g_getcmp_budget > 0 && vm) {   // f=24 attack-velocity divergence (all depths)
        char caller[48];
        topmost_nut_name((uint32_t)vm, caller, sizeof caller);
        if (__builtin_strcmp(caller, "ConvertTotalSpeed") == 0) {
            sq_getinstanceup_t p_giu = (sq_getinstanceup_t)(0x182d60_R);
            sq_getuserdata_t   p_gud = (sq_getuserdata_t)(0x1834f0_R);
            void* obj = nullptr; void* udp = nullptr;
            p_giu(vm, 1, &obj, 0);
            p_gud(vm, -1, &udp, 0);
            uint32_t off  = udp ? *(uint32_t*)udp : 0xFFFFFFFFu;
            uint32_t addr = (uint32_t)(uintptr_t)obj + off;
            uint32_t vb   = rd_ok(addr, 4) ? *(uint32_t*)(uintptr_t)addr : 0xDEADBEEFu;
            --g_getcmp_budget;
            log_printf("[getcmp] f=%d rb=%d d=%d obj=%08X off=%X addr=%08X val=%08X\n",
                       f, gekko_bridge::g_trace_rb, gekko_bridge::g_trace_depth,
                       (uint32_t)(uintptr_t)obj, off, addr, vb);
        }
    }
    return g_h_get.unsafe_ccall<int>(vm);
}

} // namespace

// Public Dr0 write-watch arm, callable from elsewhere (the [nuttrace]
// __gekko_watch_va native arms this on the player's va.x C++ address). Self-
// contained: installs the VEH + sets Dr0 on the calling (sim) thread. Arms once.
void watch_arm(uint32_t addr) { vw_arm(addr); }

void install() {
    // Investigation scaffolding — [a2dlog]/[a2dstep]/[a2dfld] divergence probes,
    // the [setwalk]/[getcmp] SQVM closure-name walk and the [velwatch] Dr0
    // write-watch that named X (= this.va.x, fixed by battle_pools::mathpool_*).
    // Spent now, so OFF by default; set SQUIROLL_A2D_DIAG=1 to re-arm them (e.g.
    // for a future per-actor divergence hunt). Keeping the install gated rather
    // than deleted preserves the named-and-commented hook code for reuse.
    if (!getenv("SQUIROLL_A2D_DIAG")) {
        log_printf("[a2dlog] investigation hooks disabled "
                   "(set SQUIROLL_A2D_DIAG=1 to enable)\n");
        return;
    }

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

    g_h_set = safetyhook::create_inline((void*)SQRAT_SET_MEMBER_VAR_FLOAT,
                                        (void*)hook_set);
    log_printf("[setwalk] hook Sqrat::SetMemberVar<float> @ 0x%X %s\n",
               (uint32_t)SQRAT_SET_MEMBER_VAR_FLOAT,
               g_h_set.enabled() ? "OK" : "FAIL");

    g_h_get = safetyhook::create_inline((void*)SQRAT_GET_MEMBER_VAR_FLOAT,
                                        (void*)hook_get);
    log_printf("[getcmp] hook Sqrat::GetMemberVar<float> @ 0x%X %s\n",
               (uint32_t)SQRAT_GET_MEMBER_VAR_FLOAT,
               g_h_get.enabled() ? "OK" : "FAIL");
}

} // namespace actor2d_log
