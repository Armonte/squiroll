// safetyhook MUST be included before any squiroll header — util.h #defines
// the calling-convention keywords as attribute macros and safetyhook uses
// those identifiers as method names. (Same ordering constraint as
// sq_arena.cpp.)
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "cpp_arena.h"
#include "crash_handler.h" // register_thread for the hardware watchpoint
#include "patch_utils.h"   // _R address literal
#include "util.h"
#include "log.h"
#include "sq_arena.h"      // sq_arena::base/capacity  (for the [arenas] dump)
#include "snapshot_ring.h" // snapshot_ring::gl_capture (game-loop render-dispatch pin)
#include "bullet_arena.h"  // bullet_arena::base/capacity

namespace actor2d_log { void watch_arm(uint32_t addr); }  // Dr0 write-watch (VEH logs writer rva)

// operator new(size_t) — th155.exe 0x2E15AB. The single scalar throwing
// operator new: std::vector / std::list (_Buynode0) / std::allocator and
// effectively every C++ heap object th155 allocates routes through it.
// Hooking it captures th155's whole C++ heap into a snapshot-able arena —
// the same model sq_arena uses for the Squirrel heap. (Hooking th155.exe's
// operator new does NOT touch squiroll/GekkoNet allocations: Netcode.dll
// has its own statically-linked CRT, so the arena stays battle-scoped.)
#define OPERATOR_NEW (0x2E15AB_R)
// _free_base — th155.exe 0x312347. The CRT free chokepoint (free / operator
// delete funnel here). Range-routed: an arena pointer goes to arena_free.
#define FREE_BASE    (0x312347_R)
// malloc — th155.exe 0x306FBC. The universal C/C++ heap chokepoint: operator
// new, operator new[] and direct malloc all funnel here. Hooked only to
// record call sites (DIAGNOSTIC) — not routed.
#define MALLOC_FN    (0x306FBC_R)
// _beginthreadex — th155.exe 0x3105AC. Every th155 worker thread (audio,
// loaders, ...) is spawned here. Hooked so each thread can be tagged by its
// start routine at creation: the audio thread is then excluded from the
// arena from boot, before it allocates anything.
#define BEGINTHREADEX (0x3105AC_R)

// Frame/rollback counters owned by gekko_bridge — used to tag connection-lifecycle
// trace lines so a forward save can be diffed against its rollback re-sim.
namespace gekko_bridge { extern int g_trace_frame; extern int g_trace_rb; }

namespace cpp_arena {
namespace {

// 128 MB. Holds th155's live C++ heap; the per-frame snapshot copies only
// [base, base+bump). Overflow falls back to the real allocator gracefully.
// Sized for the WHOLE process's C++ heap (arena armed for the process
// lifetime), not just the battle.
static constexpr uint32_t ARENA_SIZE = 128u * 1024 * 1024;
static constexpr int      CLS_MIN_SH = 4;    // 16-byte smallest block
static constexpr int      CLS_MAX_SH = 24;   // 16 MB largest block
static constexpr int      NCLS       = CLS_MAX_SH - CLS_MIN_SH + 1;
static constexpr uint32_t HDR_MAGIC  = 0x42504143;  // 'CAPB' — allocated block
static constexpr uint32_t META_MAGIC = 0x4D504143;  // 'CAPM'

// 16-byte block header; keeps the payload 16-byte aligned.
struct Hdr {
    uint32_t cls;        // size-class shift, CLS_MIN_SH..CLS_MAX_SH
    uint32_t reqsize;    // requested payload size
    uint32_t link;       // free block: free-list link (arena offset of next).
                         // allocated block: caller RVA of the operator-new /
                         // raw_alloc site — DIAGNOSTIC, lets a rollback-
                         // divergent block be attributed to the code (battle
                         // vs audio vs ...) that allocated it.
    uint32_t magic;      // HDR_MAGIC while allocated, 0 while free
};
static_assert(sizeof(Hdr) == 16, "Hdr must be 16 bytes");

// Allocator metadata at arena offset 0 — a single memcpy of [base,bump)
// captures the allocator's own state AND the whole heap, so allocation is
// bit-reproducible across a rollback re-simulation.
struct Meta {
    uint32_t magic;
    uint32_t bump;             // offset of next fresh SIM block (high-water, grows up)
    uint32_t live_bytes;       // currently-handed-out payload bytes
    uint32_t free_off[NCLS];   // per-class free-list head offset (0 = empty)
    uint32_t render_bump;      // next fresh RENDER block (grows up from RENDER_BASE)
    uint32_t reserved[7];
};

// The arena is split into a SIM region [Meta, RENDER_BASE) and a RENDER region
// [RENDER_BASE, ARENA_SIZE). Render allocs (DrawCommandSlot &c) are forward-only
// and alloc'd+freed every frame; keeping them in their OWN bump means that churn
// can NEVER move the sim's boost::signals2 connection nodes (the cpp-divergence
// root). Both regions are dirty-page captured+restored, so the sim's refs into the
// render region stay valid; cpp is excluded from the desync checksum so the render
// region diverging fwd-vs-resim is harmless. 16 MB is ample for per-frame render.
static constexpr uint32_t RENDER_BASE = 112u * 1024 * 1024;

static uint8_t* g_base      = nullptr;
static Meta*    g_meta      = nullptr;
static bool     g_installed = false;
static bool     g_armed     = false;   // route operator new -> arena only while a match is armed
static bool     g_resim     = false;   // a rollback re-simulation advance is in progress
static uint32_t g_warn      = 8;
static uint32_t g_resim_skips = 0;     // real-heap frees suppressed during re-sim
static uint32_t g_xthr_skips  = 0;     // arena frees from a non-sim thread, leaked
static DWORD    g_sim_tid   = 0;       // simulation thread; once set, ONLY this
                                       // thread's operator new -> arena (keeps
                                       // the audio thread out of the snapshot)

// DIAGNOSTIC: caller of the in-flight operator new, + a quota for logging
// size-class-13 (8 KB) bump-allocations — the rollback re-sim does one extra,
// diverging cpp_arena (the DESYNC frame=11 root).
static uint32_t g_opnew_caller = 0;
static int      g_cls13_log    = 0;   // [cpp13] dump DISABLED (cpp excluded from checksum)

// DIAGNOSTIC (Phase 1, post-input-fix residual): per-advance count of the two
// divergent allocators — DrawCommandSlot reset (0x57DC0) and boost::signals2
// shared_count (0x31C00) — to test the forward-vs-re-sim asymmetry hypothesis.
static uint32_t g_diag_render = 0;
static uint32_t g_diag_signal = 0;
static uint32_t g_diag_signal_off = 0;   // signal-node allocs on a NON-sim thread

static SafetyHookInline g_h_opnew{};
static SafetyHookInline g_h_free{};
static SafetyHookInline g_h_malloc{};
static SafetyHookInline g_h_bthreadex{};
static SafetyHookInline g_h_throw{};

// _CxxThrowException(pObject, pThrowInfo) — the central MSVC C++ throw. Log every
// exception's TYPE NAME so the unhandled throw that aborts the re-sim (FASTFAIL via
// abort in RunOneFrame) is identifiable. The last [cxxthrow] before the abort is
// the culprit. 32-bit _ThrowInfo: +12 -> CatchableTypeArray; [+0]=count, [+4]=
// CatchableType*; CatchableType+4 -> TypeDescriptor; TypeDescriptor+8 -> name.
static void stdcall throw_log_hook(void* obj, void* ti) {
    static int g_throw_n = 0;
    if (ti && g_throw_n < 600) {
        const char* name = "?";
        uint32_t cta = *(uint32_t*)((char*)ti + 12);
        if (cta && *(const int*)cta > 0) {
            uint32_t ct = *(uint32_t*)(cta + 4);
            if (ct) {
                uint32_t td = *(uint32_t*)((char*)ct + 4);
                if (td) name = (const char*)(td + 8);
            }
        }
        ++g_throw_n;
        log_printf("[cxxthrow] #%d resim=%d type='%s'\n",
                   g_throw_n, (int)g_resim, name);
    }
    g_h_throw.unsafe_stdcall<void>(obj, ti);
}

// QUEUE-LEVEL FLATTEN (SQUIROLL_SYNC_WORKERS=1). Ew_sTask__SyncLayerWorkers(this,
// force_sync) dispatches the per-frame job queue. Its SYNCHRONOUS path (taken when
// force_sync!=0) calls DispatchWorkerTask() which DRAINS the global job queue INLINE
// on the calling thread, in atomic-index order — instead of SetEvent'ing the 8
// cJobThread workers to race it. Forcing force_sync=1 while armed makes every
// per-frame worker job run in a single DETERMINISTIC order, killing the broad cpp
// divergence from concurrent worker allocation interleave. Both the forward save and
// its re-sim take this path, so the dispatch is identical. (Wall-clock perf counters
// the sync path writes into sTask .data may then need an engine_snap exclusion.)
static SafetyHookInline g_h_synclayer{};
static bool g_sync_workers = false;
// Set true around render_one_frame() (the forward-only render pass — particle draws
// run SQVM scripts whose transient boost::signals2 connections leak in the arena;
// the headless re-sim skips render entirely). While set, arena allocs go to the
// RENDER region so that leak can't move the sim bump. Render pass is single-threaded
// on the main thread (and with SYNC_WORKERS the worker drain is inline too), so a
// plain global is race-free.
static bool g_render_pass = false;
// NOTE: routing the render-effect worker DISPATCH allocations to the render region
// (a g_render_depth set around SyncLayerWorkers calls from render_effect_*) made cpp
// WORSE (24->30 frames) and re-broke eng — those allocations include sim-referenced
// state, the same coupling that dangled the DrawCommandSlot refs. The render-effect
// connection nodes are forward-only render BUT live in the sim dispatch list; they
// can't be cleanly separated by alloc-site. Left out.
static thread_local int g_render_depth = 0;   // reserved; currently always 0
static void thiscall synclayer_hook(int task, int force_sync) {
    int fs = (g_sync_workers && g_armed) ? 1 : force_sync;
    g_h_synclayer.unsafe_thiscall<int>(task, fs);
}

// DIAG: Ew_draw_effect_instance (0x10A080) = the per-effect GPU draw, called inside
// the particle draw vtable[19] alongside SQVM__Call_3. Log frame/rb to settle whether
// the HEADLESS re-sim runs the effect draw at all (rb>0 = it does -> divergence is
// alloc churn; rb==0 only -> fwd/resim ASYMMETRY -> the draw's script side-effects
// must be replayed or excluded).
static SafetyHookInline g_h_draw{};
static int thiscall draw_eff_hook(int self, int fb) {
    // Log rb per call (forward nf<20, re-sim nr<200) to settle whether the re-sim
    // GPU-draws. rb>0 present => re-sim DOES draw (divergence is alloc churn, not
    // an asymmetry). rb==0 ONLY => re-sim skips the draw (asymmetry).
    static int nf = 0, nr = 0;
    int rb = gekko_bridge::g_trace_rb;
    if (rb == 0 ? (nf < 20 && ++nf) : (nr < 200 && ++nr))
        log_printf("[draweff] rb=%d\n", rb);
    static int chain = 0;
    if (chain == 0) {
        ++chain;
        volatile uint32_t marker = 0;
        const uint32_t* sp = (const uint32_t*)&marker;
        log_printf("[drawchain] (fresh) caller chain rva:\n");
        for (int k = 0, shown = 0; k < 90 && shown < 14; ++k) {
            uint32_t rva = sp[k] - (uint32_t)base_address;
            if (rva >= 0x1000 && rva < 0x300000) {
                log_printf("[drawchain]   stk[+0x%02X] rva=%08X\n", k * 4, rva);
                ++shown;
            }
        }
    }
    return g_h_draw.unsafe_thiscall<int>(self, fb);
}

// process_pending_script_frames (0x591D0) is th155's update_logic RunOneFrame path
// — the FORWARD-ONLY render/draw tick (squiroll's advance_one_frame deliberately
// skips it). Bracket it so the particle-draw allocations (the SQVM transient
// connection nodes that otherwise churn the sim bump) land in the arena's render
// region instead. This is the CORRECT bracket point (render_one_frame was not — the
// draws are slots under THIS RunOneFrame, not drawing_related).
// DISABLED bracket: routing process_pending_script_frames' allocs to the render
// region made cpp 24->38 (it allocates sim-referenced render-ScriptAPI state, not
// just the transient draw nodes). Routing is dead. Hook kept as a no-op anchor.
static SafetyHookInline g_h_pendframes{};
static void thiscall pendframes_hook(int self) {
    g_h_pendframes.unsafe_thiscall<int>(self);
}

// LOG: generate_trail_mesh (0x159320) writes the trail vertices into *(this+164).
// Confirm (a) does the re-sim call it (rb>0?), (b) is *(this+164) the diverging cpp
// region (~off 0x5B6C000 = the per-frame dynamic vertex-buffer ring), (c) the vec at
// +168 and the count at +160. This settles whether the divergence is the render
// dynamic-VB ring (per-frame scratch that the re-sim never writes) sitting in the snap.
// FIX: set_number_digit_display (0x158A40) rebuilds a TF4_Number's per-digit quad
// buffer (in the cpp arena = snapshotted) from the number's value field (in the object
// at 0x1AF0xxxx = real heap, NOT snapshotted / NOT rolled back). On a rollback re-sim
// the restored buffer is already the forward's frame-N digits, but the script (running
// in RunOneFrame) calls SetValue with the non-rolled-back, divergent value and rewrites
// the buffer -> the SOLE residual cpp divergence (a HUD number, not gameplay; sq/bt are
// clean). Suppress the rebuild during re-sim: the buffer stays consistent with the
// snapshot, and the next forward render rebuilds it for display. SQUIROLL_NUMDIG_FIX=0
// disables (default on).
// GetFPS (0xEA20) returns dword_4D9F0C[891] — a real-time FPS counter the render loop
// updates. The HUD FPS overlay (gauge_vs.nut: this.fps.SetValue(::GetFPS())) feeds it
// into a TF4_Number whose digit buffer IS in the snapshotted cpp arena, so the headless
// re-sim (reading the live, non-rolled-back counter at varying real-times) writes
// different digits -> the residual cpp divergence (proven via Dr0 -> set_number_digit_
// display -> gauge_vs.nut FPS). Force GetFPS deterministic; the overlay just freezes.
// HUD-number geometry exclusion. Every TF4_Number's per-digit quad buffer (this+84,
// up to this+108 digits * 80 bytes) is render-derived display geometry living in the
// cpp arena. The number OBJECT (incl. value/refcounts) lives in a non-rolled-back
// real-heap NetworkNode pool, so rolling back only the geometry makes them inconsistent
// -> residual cpp divergence (combo/damage at f=27+) + likely the shared_ptr-deleter
// crash. Record these pages here (set_number_digit_display sees every number each frame)
// and EXCLUDE them from snapshot capture/restore + the divergence diagnostic, so the
// numbers are fully out of rollback (re-rendered each frame from the live value).
static constexpr uint32_t ARENA_PAGES = ARENA_SIZE / 4096u;
static uint32_t g_num_pages[ARENA_PAGES / 32];   // bitset of excluded cpp pages
static SafetyHookInline g_h_numdigit{};
static void thiscall numdigit_hook(int self, int value) {
    uint32_t buf  = *(uint32_t*)(self + 84);
    uint32_t base = (uint32_t)(uintptr_t)g_base;
    if (base && buf >= base && buf < base + ARENA_SIZE) {
        uint32_t bytes = (*(uint32_t*)(self + 108) + 2u) * 80u;   // digits*80 + slack
        uint32_t p0 = (buf - base) / 4096u;
        uint32_t p1 = (buf - base + bytes) / 4096u;
        for (uint32_t pg = p0; pg <= p1 && pg < ARENA_PAGES; ++pg)
            g_num_pages[pg >> 5] |= (1u << (pg & 31));
    }
    g_h_numdigit.unsafe_thiscall<void>(self, value);
}

// Our own DLL's loaded [base, base+size) range. A ScriptAPI `this` can NEVER live
// inside Netcode.dll — if RunOneFrame is invoked with such a `this`, it is a
// corrupted boost::signals2 slot dispatch (a connection node whose bound target
// resolved to {0x2FAD0, &g_h_runone}). Computed once from an address known to be
// in our .text.
static uint32_t g_self_lo = 0, g_self_hi = 0;
static void compute_self_range() {
    if (g_self_hi) return;
    HMODULE h = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&compute_self_range, &h) && h) {
        const uint8_t* b = (const uint8_t*)h;
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)b;
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(b + dos->e_lfanew);
        g_self_lo = (uint32_t)(uintptr_t)b;
        g_self_hi = g_self_lo + nt->OptionalHeader.SizeOfImage;
    }
}
static inline bool self_is_ours(uint32_t p) {
    return g_self_hi && p >= g_self_lo && p < g_self_hi;
}

// I2 PROBE: a ScriptAPI dispatched via a SCRIPT-HELD pointer must be sim state —
// if it lives in the RENDER bump region (which leaks forward and REWINDS on
// rollback), its memory gets reused by later render allocs and the dispatch
// crashes (the +0x305AE READ-of-0xFFFFFF08: first dword = an RGBA color).
// Log the block header (self-16) once per unique offset: link = the allocating
// caller RVA -> names the misrouted allocation site so its routing can be fixed.
static void probe_render_region_dispatch(int self, const char* who) {
    if (!g_base) return;
    uint32_t gb = (uint32_t)(uintptr_t)g_base;
    if ((uint32_t)self < gb + RENDER_BASE || (uint32_t)self >= gb + ARENA_SIZE) return;
    static uint32_t seen[24]; static int nseen = 0;
    uint32_t off = (uint32_t)self - gb;
    for (int k = 0; k < nseen; ++k) if (seen[k] == off) return;
    if (nseen >= 24) return;
    seen[nseen++] = off;
    const uint32_t* h = (const uint32_t*)(uintptr_t)((uint32_t)self - 16);
    log_printf("[rrdisp] %s on RENDER-REGION obj self=%08X off=0x%X "
               "hdr{cls=%u req=%u link_rva=%05X magic=%08X} [0]=%08X f=%d rb=%d\n",
               who, (uint32_t)self, off, h[0], h[1], h[2], h[3],
               *(const uint32_t*)(uintptr_t)self,
               gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb);
}

// DIAG: which Act::ScriptAPI::RunOneFrame (0x2FAD0) invokes the forward-only draw
// slots? Log `this` + rb. A `this` that appears with rb==0 ONLY is the draw/render
// ScriptAPI the re-sim skips (advance_one_frame's main+bg run on both rb=0 and rb>0).
static SafetyHookInline g_h_runone{};
static void thiscall runone_hook(int self) {
    static int n = 0;
    // ROLLBACK CRASH GUARD: a corrupted boost::signals2 slot fires RunOneFrame with
    // this == &g_h_runone (our own SafetyHookInline, in Netcode.dll). The body then
    // walks our hook object as a ScriptAPI signal and does `lock inc` on a read-only
    // .text address (control-block field = &runone_hook) -> 0xC0000005. That `this`
    // is never a real ScriptAPI, so skip the call entirely (the corrupt dispatch is
    // a no-op instead of a crash; render self-corrects next forward frame).
    compute_self_range();
    if (self_is_ours((uint32_t)self) || (uint32_t)self < 0x10000) {
        static int badn = 0;
        if (badn < 40) { ++badn;
            log_printf("[runone] SKIP bad this=%08X rb=%d f=%d [0]=%08X [4]=%08X (in-our-dll=%d)\n",
                       (uint32_t)self, gekko_bridge::g_trace_rb, gekko_bridge::g_trace_frame,
                       (uint32_t)self >= 0x10000 ? *(uint32_t*)self : 0,
                       (uint32_t)self >= 0x10000 ? *(uint32_t*)(self + 4) : 0,
                       (int)self_is_ours((uint32_t)self)); }
        return;
    }
    probe_render_region_dispatch(self, "RunOneFrame");
    // One-shot: are the main game-loop ScriptAPI (g_gameloop_scriptapi @0x49AFBC) and
    // the sim ScriptAPI (advance_one_frame drives *0x49B01C) the SAME object? Decides
    // whether the crash's render-connection list can be excluded as a whole ScriptAPI.
    static bool dumped_api = false;
    if (!dumped_api) {
        dumped_api = true;
        log_printf("[api] gameloop_api=%08X sim_api=%08X this=%08X\n",
                   *(uint32_t*)(0x49AFBC_R), *(uint32_t*)(0x49B01C_R), (uint32_t)self);
    }
    // Log a few of EACH: forward (rb==0) and re-sim (rb>0). The ScriptAPI `this`
    // that appears under rb==0 but NEVER under rb>0 is the forward-only draw path.
    static int nf = 0, nr = 0;
    int rb = gekko_bridge::g_trace_rb;
    if (rb == 0 ? (nf < 40 && ++nf) : (nr < 200 && ++nr)) {
        log_printf("[runone] this=%08X rb=%d\n", (uint32_t)self, rb);
    }
    (void)n;
    // NB: tried routing the game-loop ScriptAPI's (g_gameloop_scriptapi @0x49AFBC, the
    // FORWARD-ONLY render dispatch — separate from the re-sim's sim ScriptAPI @0x49B01C)
    // RunOneFrame allocations to the render region + skipping that region from the
    // snapshot, to take render-dispatch state out of rollback (the crash = its restored
    // connection list referencing freed objects). But the SIM references render objects,
    // so excluding the region broke sq/eng. Render/sim aren't page-separable. Reverted.
    // NB: routing the game-loop's whole RunOneFrame to the render region broke the SIM
    // (sqDIV) — it allocates sq-referenced state too, not just render-dispatch. Reverted.
    // Snapshot the render-dispatch graph at the stable RunOneFrame-end; the collision with
    // re-simmed sim objects is handled by an identity check in gl_pin_restore.
    g_h_runone.unsafe_thiscall<int>(self);
    if ((uint32_t)self == *(uint32_t*)(0x49AFBC_R)) snapshot_ring::gl_capture();
}

// DIAG: confirm the slot-list shared_count divergence. signal_lock_slot_list (0x304F0)
// reads the signal's slot-list shared_ptr from the ScriptAPI object: this+0 = list head,
// this+4 = shared_count control block (sc+4 = use_count, sc+8 = weak_count). The crash
// (HasPendingFrame 0x2FA7E) fires only if use_count is driven to 0 by the rollback. Log
// the game-loop ScriptAPI's signal state at HasPendingFrame entry, tagged frame/rb, to
// catch use_count dropping to 0 (and the exact sc address to decouple).
static SafetyHookInline g_h_haspend{};
static char thiscall haspend_hook(int self) {
    // Same corrupted-slot guard as runone_hook: a this inside our own DLL is not a
    // real ScriptAPI; calling the original would `lock inc` a read-only .text addr.
    compute_self_range();
    if (self_is_ours((uint32_t)self) || (uint32_t)self < 0x10000) {
        static int badn = 0;
        if (badn < 20) { ++badn;
            log_printf("[haspend] SKIP bad this=%08X rb=%d f=%d\n", (uint32_t)self,
                       gekko_bridge::g_trace_rb, gekko_bridge::g_trace_frame); }
        return 0;
    }
    probe_render_region_dispatch(self, "HasPendingFrame");
    uint32_t head = *(uint32_t*)(self + 0);
    uint32_t sc   = *(uint32_t*)(self + 4);
    uint32_t gb = (uint32_t)(uintptr_t)g_base;
    bool in_arena = (sc >= gb && sc < gb + ARENA_SIZE);
    int use = -2, weak = -2;
    if (in_arena) { use = *(int*)(sc + 4); weak = *(int*)(sc + 8); }
    bool gl = ((uint32_t)self == *(uint32_t*)(0x49AFBC_R));
    // SELF-HEAL (2026-07-01): a dead control block (use<=0) on the PERSISTENT
    // game-loop signal is always corruption — entering the original would lock
    // (0->1), release (1->0) and dispose a live slot list, then spin forever in
    // the catch-up loop (the 5/10 hang). The signal's rest state is use=1
    // weak=1 by construction (it owns exactly one ref); restore it and let the
    // dispatch proceed. Loud log so occurrences stay visible; with the restore
    // step-0 fix + preservation removed this should fire rarely or never.
    if (gl && in_arena && use <= 0) {
        *(int*)(sc + 4) = 1;
        *(int*)(sc + 8) = 1;
        static int nheal = 0;
        if (nheal < 60) { ++nheal;
            log_printf("[haspend] SELF-HEAL gl sc=%08X use %d->1 weak %d->1 f=%d rb=%d\n",
                       sc, use, weak, gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb); }
        use = 1; weak = 1;
    }
    // DANGER = the release decrement will hit 0 (use<=0 going in) or sc is a bad ptr.
    // use==1 is HEALTHY (signal holds 1 ref). Log every danger (any ScriptAPI).
    bool danger = (!in_arena) || (use <= 0);
    static int n = 0;
    if (n < 600 && danger) {
        ++n;
        log_printf("[haspend] f=%d rb=%d self=%08X%s head=%08X sc=%08X(%s) use=%d weak=%d  <-DANGER\n",
                   gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb, (uint32_t)self,
                   gl ? "*GL" : "", head, sc, in_arena ? "arena" : "OUT", use, weak);
    }
    (void)head;
    return g_h_haspend.unsafe_thiscall<char>(self);
}

// concurrent_list_erase_node (0x13D80) — __thiscall(container, out, key, node_ref).
// RunOneFrame calls this to erase a disconnected boost::signals2 connection from
// the ScriptAPI per-frame dispatch list. The list/cursor diverging across the
// rollback re-sim is the cpp root. Log every erase tagged with frame/rb/thread +
// the string key + node addr, so a forward run can be diffed against its re-sim to
// find the connection that erases on a DIFFERENT frame (and whether a worker thread
// is doing it).
static SafetyHookInline g_h_erase{};
static uint32_t* thiscall erase_log_hook(int* self, uint32_t* out, int* key, int** node) {
    void* nodeaddr = node ? (void*)*node : nullptr;
    // MSVC std::string key: size at +16; SSO buffer at +0, else heap ptr at +0.
    const char* ks = (const char*)key;
    if (key && ((const uint32_t*)key)[4] >= 16) ks = *(const char**)key;
    uint32_t* r = g_h_erase.unsafe_thiscall<uint32_t*>(self, out, key, node);
    static int n = 0;
    if (n < 40000) {
        ++n;
        log_printf("[erase] f=%d rb=%d tid=%u node=%p cnt=%d key='%.24s'\n",
                   gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb,
                   GetCurrentThreadId(), nodeaddr, self ? self[1] : -1,
                   ks ? ks : "?");
    }
    return r;
}

// Threads whose operator new must NEVER reach the arena — the audio thread
// above all. Populated by the _beginthreadex hook from each thread's start
// routine, so the exclusion is live from the thread's first instruction.
static DWORD    g_excl_tid[16] = {0};
static int      g_n_excl_tid   = 0;
// Start-routine RVA window that marks a thread as audio/non-sim. Filled in
// from the [thread] log; 0,0 = match nothing (log-only first pass).
static uint32_t g_audio_lo = 0;
static uint32_t g_audio_hi = 0;

// "Deterministic workers" — additional threads beyond g_sim_tid that ARE
// allowed to allocate into the arena. The Ew::cJobThread workers spawned
// via Manbow__EwCJobThread__StartAddress (RVA 0x1205A0) handle per-frame
// task dispatch and DO mutate game state (boost::signals2 grouped_list
// connection inserts, etc). Until 2026-05-27 these were locked out of the
// arena by the sim-only gate, so their allocations landed in the real
// Win32 heap which is NOT snapshotted — that's the source of the freeze-
// on-hit during rollback re-sim: re-sim's boost::signals2 mutations
// duplicated the forward-sim state, eventually corrupting the
// grouped_list trees with cycles.
//
// We identify these threads at _beginthreadex time by matching start
// against the cJobThread StartAddress thunk. Once added here, the same
// gate that admits the sim thread admits these too.
static DWORD    g_worker_tid[64] = {0};
static int      g_n_worker_tid   = 0;
// Start-routine RVA window for the cJobThread StartAddress thunk.
// 0x1205A0 is the single entry point (StartAddress in IDB) — we match
// it exactly. lo<=srva<hi range is left for any future cohort that needs
// wider matching.
static uint32_t g_worker_start_lo = 0x1205A0;
static uint32_t g_worker_start_hi = 0x1205A1;

static bool thread_excluded(DWORD tid) {
    for (int i = 0; i < g_n_excl_tid; ++i)
        if (g_excl_tid[i] == tid) return true;
    return false;
}

static bool thread_is_worker(DWORD tid) {
    for (int i = 0; i < g_n_worker_tid; ++i)
        if (g_worker_tid[i] == tid) return true;
    return false;
}

// The arena is armed for the whole process lifetime and th155 hits operator
// new / _free_base from MULTIPLE THREADS (background resource loading). The
// bump pointer and per-class free lists are not atomic, so arena_alloc and
// arena_free serialise on this lock. A Win32 CRITICAL_SECTION is recursive,
// so nested entry (any future arena_realloc -> arena_alloc) is safe.
// Initialised by install() before the hooks go live.
static CRITICAL_SECTION g_lock;

// DIAGNOSTIC: ring of recent th155 malloc() calls (caller RVA + size + ptr),
// recorded only while armed. trace_alloc() walks it to attribute a divergent
// pointer to the exact allocation call site.
struct MallocRec { uint32_t caller; uint32_t size; uint32_t ptr; };
static constexpr int MW_RING = 16384;
static MallocRec  g_mw[MW_RING];
static uint32_t   g_mw_idx = 0;

// DIAGNOSTIC: per-frame arena alloc/free sequence trace. Records every
// arena event of a chosen frame so the forward run and a rollback re-sim
// of that SAME frame can be diffed event-by-event — the first mismatch is
// the exact point the re-sim deviates. See trace_reset / trace_check.
struct TraceEv { uint8_t op; uint32_t size, caller, off; };  // op 1=alloc 2=free
static constexpr uint32_t TRACE_CAP   = 65536;
static TraceEv  g_tr[TRACE_CAP];          // the current advance's events
static uint32_t g_tr_n        = 0;
// Per-frame ring of forward alloc sequences: a re-sim of ANY frame diffs
// against that frame's forward sequence, so the divergence ONSET frame is
// found automatically (it is run-variable). RING > rollback window (8).
static constexpr uint32_t TR_RING     = 12;
static constexpr uint32_t TR_SLOT_CAP = 8192;
static TraceEv  g_tr_ring[TR_RING][TR_SLOT_CAP];
static uint32_t g_tr_ring_n[TR_RING]     = {0};
static int32_t  g_tr_ring_frame[TR_RING];          // frame in each slot (-1 = empty)
static bool     g_tr_ring_init = false;
static int      g_tr_div_log   = 8;                // log the first N divergent frames

static void trace_rec(uint8_t op, uint32_t size, uint32_t caller, uint32_t off) {
    if (g_tr_n < TRACE_CAP) {
        TraceEv& e = g_tr[g_tr_n++];
        e.op = op; e.size = size; e.caller = caller; e.off = off;
    }
}

// Smallest class shift whose block (1<<sh) holds sizeof(Hdr)+n.
static int class_for(size_t n) {
    size_t need = n + sizeof(Hdr);
    int sh = CLS_MIN_SH;
    while (((size_t)1 << sh) < need) {
        if (sh >= CLS_MAX_SH) return CLS_MAX_SH + 1;  // too big for the arena
        ++sh;
    }
    return sh;
}

static void* arena_alloc(size_t n, bool is_render) {
    int sh = class_for(n);
    if (sh > CLS_MAX_SH) return nullptr;
    int      ci  = sh - CLS_MIN_SH;
    uint32_t blk = 1u << sh;
    uint32_t off;
    // Lock spans every read/write of g_meta->bump and g_meta->free_off[].
    EnterCriticalSection(&g_lock);
    if (is_render && (uint64_t)g_meta->render_bump + blk <= ARENA_SIZE) {
        // RENDER region bump (separate from the sim — see RENDER_BASE). Leaks like
        // the sim (no recycle); 16 MB holds plenty of per-frame render churn.
        off = g_meta->render_bump;
        g_meta->render_bump += blk;
    } else if (g_meta->free_off[ci]) {
        off = g_meta->free_off[ci];
        g_meta->free_off[ci] = ((Hdr*)(g_base + off))->link;
    } else {
        if ((uint64_t)g_meta->bump + blk > RENDER_BASE) {
            if (g_warn) {
                --g_warn;
                log_printf("[cpp_arena] !! SIM region FULL bump=%u +%u (cap=%u)\n",
                           g_meta->bump, blk, RENDER_BASE);
            }
            LeaveCriticalSection(&g_lock);
            return nullptr;
        }
        off = g_meta->bump;
        g_meta->bump += blk;
        // DIAGNOSTIC: cls-13 (8 KB) bump-grows are the cpp_arena divergence.
        // Scan the stack for th155 return addresses to get the call chain.
        if (sh == 13 && g_cls13_log > 0) {
            --g_cls13_log;
            log_printf("[cpp13] BUMP cls13 size=%u caller_rva=%08X bump->%u "
                       "chain:\n", (uint32_t)n,
                       g_opnew_caller - (uint32_t)base_address, g_meta->bump);
            const uint32_t* sp = (const uint32_t*)&blk;
            for (int k = 0; k < 110; ++k) {
                uint32_t rva = sp[k] - (uint32_t)base_address;
                if (rva >= 0x1000 && rva < 0x300000)
                    log_printf("[cpp13]   stack[+0x%02X] rva=%08X\n", k * 4, rva);
            }
        }
    }
    Hdr* h = (Hdr*)(g_base + off);
    h->cls       = (uint32_t)sh;
    h->reqsize   = (uint32_t)n;
    h->link      = g_opnew_caller
                 ? (uint32_t)(g_opnew_caller - (uint32_t)base_address) : 0;
    h->magic     = HDR_MAGIC;
    g_meta->live_bytes += (uint32_t)n;
    trace_rec(1, (uint32_t)n, h->link, off);
    LeaveCriticalSection(&g_lock);
    return g_base + off + sizeof(Hdr);
}

static void arena_free(void* p) {
    Hdr* h = (Hdr*)((uint8_t*)p - sizeof(Hdr));
    // Lock spans every read/write of g_meta->free_off[] (and the header
    // mutation that links the block onto a free list).
    EnterCriticalSection(&g_lock);
    if (h->magic != HDR_MAGIC) {
        if (g_warn) {
            --g_warn;
            log_printf("[cpp_arena] !! free of bad/double block magic=%08x\n",
                       h->magic);
        }
        LeaveCriticalSection(&g_lock);
        return;
    }
    int ci = (int)h->cls - CLS_MIN_SH;
    if (ci < 0 || ci >= NCLS) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    g_meta->live_bytes -= h->reqsize;
    uint32_t foff = (uint32_t)((uint8_t*)h - g_base);
    trace_rec(2, h->reqsize, 0, foff);
    // EXPERIMENTAL — option (ii): instead of putting the block on its
    // size-class free list, LEAK it. arena_alloc always bumps to fresh
    // memory, no recycle. Used to test the hypothesis that cpp_arena's
    // deterministic recycle exposes a latent use-after-free in TH155
    // (a freed list node gets reused immediately and the new owner's
    // bytes look like list pointers when the stale list walks it).
    //
    // If this fixes the hit-crash, the recycle is the cause and we
    // need either a quarantine (delay recycle N frames) or to find
    // the offending freer. Memory cost is real — full 128MB capacity
    // gets exhausted faster — but should comfortably last one match.
    // Zero-fill the payload too so any stale read sees null, not the
    // previous live data.
    memset(p, 0, h->reqsize);
    h->magic     = 0;
    // Free-list link NOT updated: block stays orphaned, arena_alloc
    // will skip the free list and bump.
    (void)foff;
    (void)ci;
    LeaveCriticalSection(&g_lock);
}

static inline bool in_arena(const void* p) {
    return p && (const uint8_t*)p > g_base
             && (const uint8_t*)p < g_base + ARENA_SIZE;
}

// --- hooks --------------------------------------------------------------

// Caller RVA ranges whose operator-new allocations must NOT enter the arena.
// The arena is the rollback snapshot; an object a non-deterministic subsystem
// (the audio thread) mutates would make the snapshot diverge on a re-sim.
// These ranges are found with cpp_arena::attribute() — it names the caller of
// any block that diverges, so this list grows from evidence, not guesses.
struct ExclRange { uint32_t lo, hi; };   // RVA half-open [lo, hi)
static const ExclRange g_excl[] = {
    // tf4_ogg_alloc_shared — operator new shared_ptr<TF4::Ogg>, the Ogg/Vorbis
    // audio decoder object; the audio thread mutates it every frame.
    { 0x16B380u, 0x16B400u },
    // NOTE: routing the RENDER allocators (DrawCommandSlot create_and_bind
    // 0x56A90-0x56AD0, reset 0x57DC0) to the real heap DID make f=2 deterministic
    // (the free-list churn that pushed the sim's connection nodes to different
    // slots — object B at re-sim offset 0x27FAA70 vs forward 0x5B5C670, [divword]),
    // and sq/bt stayed clean. BUT the arena's boost::signals2 connection nodes
    // REFERENCE those render objects, so excluding them leaves dangling refs: the
    // re-sim's RunOneFrame cleanup then frees the (real-heap) render objects, which
    // the re-sim free-suppressor swallows -> they leak (618k frees) -> OOM. So the
    // render objects MUST stay in the arena; the real fix is a SEPARATE arena
    // sub-region/free-list for render allocs so their churn can't move the sim's
    // nodes. Left out until that's built.
};
static bool caller_excluded(uint32_t abs_caller) {
    uint32_t rva = abs_caller - (uint32_t)base_address;
    for (const ExclRange& e : g_excl)
        if (rva >= e.lo && rva < e.hi) return true;
    return false;
}

// True for the FORWARD-ONLY render allocators (DrawCommandSlot create_and_bind /
// reset). Their objects go to the arena's separate RENDER region so their per-frame
// alloc/free churn can't move the sim's connection nodes (the cpp-divergence root).
static bool is_render_caller(uint32_t abs_caller) {
    uint32_t rva = abs_caller - (uint32_t)base_address;
    return (rva >= 0x56A90u && rva < 0x56AD0u)    // Manbow::DrawCommandSlot::create_and_bind
        || (rva >= 0x57DC0u && rva < 0x57E00u);   // DrawCommandSlot reset
}

// operator new: while a rollback session is armed, serve from the arena so
// the battle's C++ heap is part of the snapshot. Outside a match (boot,
// menus) pass straight to the real allocator — that keeps the arena, hence
// the per-frame snapshot, bounded to battle-era allocations. A caller in the
// excluded set (audio) also passes through: its objects are non-deterministic
// and must stay out of the snapshot. On overflow likewise fall through.
static void* cdecl hook_op_new(size_t size) {
    if (g_armed && g_meta) {
        uint32_t caller = (uint32_t)(uintptr_t)_ReturnAddress();
        g_opnew_caller = caller;
        // DIAGNOSTIC: tally the residual render/signal allocators (Phase 1).
        uint32_t crva = caller - (uint32_t)base_address;
        // render = DrawCommandSlot create site (Manbow__DrawCommandSlot__create_and_bind);
        // signal = boost::signals2 grouped connection-list node alloc (_Buynode0 0x13F10).
        if (crva >= 0x56A90 && crva < 0x56AD0) ++g_diag_render;
        else if (crva >= 0x13F00 && crva < 0x13F40) {
            ++g_diag_signal;
            // Is this connection-list node allocated on the SIM thread or a
            // cJobThread WORKER? Worker-thread allocs at non-deterministic times
            // swap node addresses (same total bump) -> the residual swap.
            // (tid isn't computed until further down, so query directly here.)
            if (g_sim_tid && GetCurrentThreadId() != g_sim_tid) ++g_diag_signal_off;
            // One-shot caller-chain dump for the per-frame signals2 connection-
            // list node alloc (_Buynode0) — names the connect() SITE up the
            // stack so we can see what orders the 8-10 per-frame connections
            // (the swap source after the input fix).
            // Skip the f=0 setup burst (~1105 connections) so the chain we dump
            // is a PER-FRAME (battle) connect — the divergent kind (high-addr).
            static uint32_t sig_skip = 1150;
            static int sig_chain_log = 12;
            if (sig_skip) {
                --sig_skip;
            } else if (!g_resim && sig_chain_log > 0) {
                --sig_chain_log;
                volatile uint32_t marker = 0;
                const uint32_t* sp = (const uint32_t*)&marker;
                log_printf("[sigchain] connlist node alloc — caller chain:\n");
                for (int k = 0; k < 48; ++k) {
                    uint32_t r = sp[k] - (uint32_t)base_address;
                    if (r >= 0x1000 && r < 0x300000)
                        log_printf("[sigchain]   stk[+0x%02X] rva=%08X\n", k * 4, r);
                }
            }
        }
        // Thread gate: once the simulation thread is known, only IT may draw
        // from the arena. Background threads — the audio thread above all —
        // go to the real heap; their non-deterministic alloc/free would
        // otherwise churn the arena free-lists and shift where battle objects
        // land, diverging the rollback snapshot. A thread tagged at creation
        // (the _beginthreadex hook) is excluded even before the sim thread is
        // known, so audio objects never enter the baseline.
        // STRICT: only the simulation thread, and only once it is known.
        // Before that (boot / engine init) operator new goes to the real
        // heap — those objects are not battle state. better_game_loop sets
        // the sim thread very early, before menus / vs.Initialize.
        DWORD tid = GetCurrentThreadId();
        bool sim    = (g_sim_tid != 0) && (tid == g_sim_tid);
        bool worker = thread_is_worker(tid);
        // PRE-GATE window: between cpp_arena::install() and
        // set_sim_thread(), g_sim_tid is 0. Without this branch the
        // sim-only gate rejects EVERY allocation in that window, so
        // bootstrap objects (boost::signals2 signal owners, the
        // grouped_list_state structs the Actor2D render slots connect
        // INTO, font tables, etc.) land in the real Win32 heap and
        // permanently escape the rollback snapshot. That's the source
        // of the freeze-on-hit: re-sim mutates the in-arena connection
        // node list, but the signal owner that POINTS into that list
        // is at a fixed real-heap address holding stale forward-sim
        // pointers — eventually a cycle, then the walk loops forever.
        //
        // Pre-gate we admit ALL threads (still respecting excluded
        // sets / callers). The arena is 128 MB which comfortably
        // covers boot. Once sim_tid is set the gate narrows to
        // sim + workers + non-excluded — same as before.
        bool pre_gate = (g_sim_tid == 0);
        bool admitted = sim || worker || pre_gate;
        if (admitted && !thread_excluded(tid) && !caller_excluded(caller)) {
            void* p = arena_alloc(size, is_render_caller(caller) || g_render_depth > 0
                                        || (g_sync_workers && g_render_pass));
            if (p) return p;
        }
    }
    return g_h_opnew.unsafe_ccall<void*>(size);
}

// _free_base: an arena pointer is detected by address range — exact, no
// caller check, and arena_free IS rolled back with the snapshot.
//
// A NON-arena (real Win32 heap) pointer is the problem: the real heap is not
// part of the rollback snapshot. During a rollback re-simulation the forward
// run has ALREADY freed that block — re-freeing it is a double-free, which
// RtlFreeHeap turns into a STATUS_HEAP_CORRUPTION fastfail. So a real-heap
// free issued from inside a re-sim advance is suppressed (the block stays
// freed from the forward pass; the bounded leak is the transient buffers a
// growing std::vector sheds — they stop once capacity settles).
static void cdecl hook_free(void* block) {
    if (in_arena(block)) {
        // Non-sim, non-worker threads (the audio thread) freeing an arena
        // block — necessarily a pre-gate baseline object — would push it
        // onto a free-list at a non-deterministic time, shifting where the
        // next simulation allocation lands and diverging the snapshot.
        // Leak it instead; the set of such blocks is bounded (pre-gate
        // only). Worker threads (Ew::cJobThread) ARE allowed to free —
        // their allocations come from the arena too and live in the
        // rollback snapshot just like the sim thread's.
        DWORD tid = GetCurrentThreadId();
        bool admitted = (g_sim_tid == 0)                       // pre-gate
                     || (g_sim_tid != 0 && tid == g_sim_tid)   // sim
                     || thread_is_worker(tid);                 // worker
        if (g_sim_tid != 0 && !admitted) {
            if (g_warn && (g_xthr_skips & 0xFF) == 0)
                log_printf("[cpp_arena] off-thread arena free leaked %p (#%u)\n",
                           block, g_xthr_skips);
            ++g_xthr_skips;
            return;
        }
        arena_free(block);
        return;
    }
    if (g_resim) {
        // DIAGNOSTIC: log caller RVA every 64 suppressions so we can
        // identify the divergence sources (real-heap frees that the
        // forward run does but the re-sim suppresses). The forward run's
        // matching free DOES happen — the asymmetry is exactly here.
        if ((g_resim_skips & 0x3F) == 0) {
            uint32_t caller = (uint32_t)(uintptr_t)_ReturnAddress();
            log_printf("[cpp_arena] re-sim: suppressed real-heap free %p "
                       "(#%u) by rva=%08X\n", block, g_resim_skips,
                       caller - (uint32_t)base_address);
        }
        ++g_resim_skips;
        return;
    }
    g_h_free.unsafe_ccall<void>(block);
}

// malloc: pure diagnostic — call the original, then record {caller,size,ptr}
// in the ring while armed. The caller address is malloc's return address
// (safetyhook inline preserves the original call frame).
static void* cdecl hook_malloc(size_t size) {
    void* p = g_h_malloc.unsafe_ccall<void*>(size);
    if (g_armed) {
        uint32_t i = (g_mw_idx++) & (MW_RING - 1);
        g_mw[i].caller = (uint32_t)(uintptr_t)_ReturnAddress();
        g_mw[i].size   = (uint32_t)size;
        g_mw[i].ptr    = (uint32_t)(uintptr_t)p;
    }
    return p;
}

// FLATTEN (SQUIROLL_FLATTEN_JOBS=1): run each one-shot Ew::cJobThread job INLINE on
// the dispatching thread instead of on its own worker thread, so the per-frame jobs
// (which allocate boost::signals2 / sim objects into cpp_arena) run in a single
// DETERMINISTIC ORDER — killing the residual broad cpp divergence from concurrent
// worker allocation interleaving. A no-op thread is still spawned so the dispatcher
// gets a real, immediately-signaled handle to join/close.
static bool g_flatten_jobs = false;
static unsigned stdcall noop_thread_start(void*) { return 0; }

// _beginthreadex: tag every th155 worker thread by its start routine at
// creation. A thread whose start routine falls in the audio window is
// excluded from the arena before it runs a single instruction, so no audio
// object ever enters the rollback snapshot — not even in the baseline.
static uintptr_t cdecl hook_beginthreadex(void* sec, unsigned stk, void* start,
                                          void* arg, unsigned flag,
                                          unsigned* tidp) {
    uint32_t srva0 = (uint32_t)((uintptr_t)start - base_address);
    if (g_flatten_jobs && (g_worker_start_hi > g_worker_start_lo) &&
        srva0 >= g_worker_start_lo && srva0 < g_worker_start_hi &&
        !(flag & CREATE_SUSPENDED)) {
        // Run the job NOW, in dispatch order, on this thread.
        ((unsigned (stdcall*)(void*))start)(arg);
        // Hand back a real handle (no-op thread) so the dispatcher's wait/close works.
        return g_h_bthreadex.unsafe_ccall<uintptr_t>(sec, stk,
                   (void*)noop_thread_start, nullptr, flag, tidp);
    }
    uintptr_t h = g_h_bthreadex.unsafe_ccall<uintptr_t>(sec, stk, start, arg,
                                                        flag, tidp);
    if (h) {
        DWORD tid = (tidp && *tidp) ? (DWORD)*tidp
                                    : GetThreadId((HANDLE)h);
        uint32_t srva = (uint32_t)((uintptr_t)start - base_address);
        bool audio = (g_audio_hi > g_audio_lo) &&
                     (srva >= g_audio_lo && srva < g_audio_hi);
        // Ew::cJobThread workers spawn at Manbow__EwCJobThread__StartAddress
        // (0x1205A0). These are deterministic per-frame task workers and
        // their allocations MUST land in cpp_arena so the rollback snapshot
        // covers them (the freeze-on-hit issue was their boost::signals2
        // grouped_list inserts landing on the un-snapshotted real heap).
        bool worker = (g_worker_start_hi > g_worker_start_lo) &&
                      (srva >= g_worker_start_lo && srva < g_worker_start_hi);
        if (audio && g_n_excl_tid < 16) g_excl_tid[g_n_excl_tid++] = tid;
        if (worker && g_n_worker_tid <
                          (int)(sizeof(g_worker_tid) / sizeof(g_worker_tid[0])))
            g_worker_tid[g_n_worker_tid++] = tid;
        crash_handler::register_thread((uint32_t)tid);
        log_printf("[cpp_arena] thread spawned tid=%u start_rva=%08X%s%s\n",
                   tid, srva,
                   audio  ? "  [excluded from arena]" : "",
                   worker ? "  [WORKER: allocations -> cpp_arena]" : "");
    }
    return h;
}

// DETERMINISM: the render-only scripts (HUD/effect) that build the boost::signals2
// render-dispatch list read real-time via two Squirrel bindings — sq_push_current_time_ms
// (0x49450 -> timeGetTime) and script_get_async_key_state (0x129D40 -> GetAsyncKeyState).
// With identical inputs the SIM is deterministic, but these make the RENDER (and thus the
// dispatch list it builds/tears-down) vary with wall-clock, so the rollback corrupts a
// different list each run and the crash frame jumps f10..f70. Hook both to frame-deterministic
// values so the whole render is reproducible — required to debug the residual rollback crash.
static SafetyHookInline g_h_scripttime{};
static SafetyHookInline g_h_scriptkey{};
typedef void (cdecl* sq_pushint_t)(void* vm, int i);
static int cdecl scripttime_hook(void* vm) {
    ((sq_pushint_t)(0x184370_R))(vm, gekko_bridge::g_trace_frame * 16);   // 16 ms/frame, deterministic
    return 1;
}
static int cdecl scriptkey_hook(void* vm) {
    ((sq_pushint_t)(0x184370_R))(vm, 0);   // no key held — deterministic render input
    return 1;
}
// Neutralize GetAsyncKeyState process-wide (return 0 = no key). The harness drives inputs via
// the fake-input generator, NOT real keys, so this is safe and removes every real-time input
// read (PrtScn poll in update_logic, debug camera, movement, script_get_async_key_state).
static short __stdcall det_getasynckeystate(int /*vKey*/) { return 0; }
// Frame-deterministic clocks: th155's animation/timer reads of timeGetTime/GetTickCount must
// advance with the LOGICAL frame (identical forward vs re-sim), not wall-clock, or the render
// dispatch the rollback corrupts varies per run. squiroll's own perf/exit timing uses
// Netcode.dll's IAT (not patched), so the wall-clock harness exit still works.
static unsigned long __stdcall det_timegettime() {
    int f = gekko_bridge::g_trace_frame; return (unsigned long)((f < 0 ? 0 : f) * 16);
}
static unsigned long __stdcall det_gettickcount() {
    int f = gekko_bridge::g_trace_frame; return (unsigned long)((f < 0 ? 0 : f) * 16);
}
// The Ew::sTask scheduler AND every effect (Ew_tEftShine/Orb/Spark/Particle/...) time their
// animations off QueryPerformanceCounter — the dominant render-dispatch nondeterminism. Make
// QPC advance with the logical frame and pin the frequency so elapsed/frame == 1/60 s exactly,
// identical forward vs re-sim. (long long* = LARGE_INTEGER::QuadPart.)
static int __stdcall det_qpc(long long* p) {
    int f = gekko_bridge::g_trace_frame; if (p) *p = (long long)(f < 0 ? 0 : f) * 100000; return 1;
}
static int __stdcall det_qpf(long long* p) { if (p) *p = 6000000; return 1; }   // 100000*60
// *** THE DOMINANT NONDETERMINISM ***: Ew_sRandom__ctor (0xE95A0) seeds its SFMT19937 RNGs from
// GetLocalTime (wYear+wMonth+...+wMilliseconds). That RNG drives every effect's randomization
// (generate_random_unit_vector3 0x10A8C0, Ew_cEwEftPGeometry*, etc.), so the whole effect/render
// stream — and the dispatch list the rollback corrupts — differs EVERY launch. Pin GetLocalTime
// to a constant SYSTEMTIME so the seed (and thus the effect RNG sequence) is identical run-to-run.
// (Only other users are cosmetic timestamps.) 8 WORDs = SYSTEMTIME.
// *** COMPREHENSIVE ADDRESS DETERMINISM ***: the Windows heap grows by reserving segments via
// ntdll!NtAllocateVirtualMemory (NOT kernel32!VirtualAlloc, and not pinned by bottom-up-ASLR-off).
// So th155's heap pools (0x0C... region, 0x00B-0x00D heap) still land at run-varying addresses,
// and every pointer to a heap object in the rollback snapshot differs run-to-run. Hook the native
// allocator and assign a DETERMINISTIC base (bump allocator) to every OS-chosen reservation, so
// th155's entire dynamic address space is reproducible. Only touches *BaseAddress==NULL reserves;
// explicit-base calls (our fixed arenas) pass through. Falls back to OS choice if our slot is taken.
static SafetyHookInline g_h_ntalloc{};
static uintptr_t g_va_next = 0x50000000;   // deterministic bump region (above image+arenas, below DLLs)
typedef long (__stdcall* NtAllocVM_t)(void*, void**, unsigned long, size_t*, unsigned long, unsigned long);
static long __stdcall ntalloc_hook(void* proc, void** base, unsigned long zbits,
                                   size_t* size, unsigned long type, unsigned long prot) {
    if (base && *base == nullptr && size && *size && (type & MEM_RESERVE) && g_va_next < 0x70000000) {
        size_t rsz = (*size + 0xFFFF) & ~(size_t)0xFFFF;
        void* tryb = (void*)((g_va_next + 0xFFFF) & ~(uintptr_t)0xFFFF);
        size_t sz2 = *size;
        long st = g_h_ntalloc.unsafe_stdcall<long>(proc, &tryb, zbits, &sz2, type, prot);
        if (st >= 0) { g_va_next = (uintptr_t)tryb + rsz; *base = tryb; *size = sz2; return st; }
        // our address was unavailable — fall through to an OS-chosen reservation
    }
    return g_h_ntalloc.unsafe_stdcall<long>(proc, base, zbits, size, type, prot);
}
static void __stdcall det_getlocaltime(unsigned short* st) {
    static int n = 0;
    if (n < 6) { ++n;
        uint32_t ra = (uint32_t)((uintptr_t)__builtin_return_address(0) - base_address);
        // caller_rva ~0xE95FE => Ew_sRandom__ctor => the RNG SEED went through our hook (pinned).
        log_printf("[det] GetLocalTime caller_rva=%08X %s\n", ra,
                   (ra >= 0xE95A0 && ra <= 0xE9736) ? "<<< RNG SEED (Ew_sRandom__ctor)" : "(cosmetic)");
    }
    if (!st) return;
    st[0]=2026; st[1]=1; st[2]=0; st[3]=1; st[4]=0; st[5]=0; st[6]=0; st[7]=0;
}

} // namespace

void install() {
    if (g_installed) return;

    {   // SQUIROLL_FLATTEN_JOBS=1 -> run cJobThread jobs inline (deterministic order)
        char fb[8] = {0};
        DWORD fn = GetEnvironmentVariableA("SQUIROLL_FLATTEN_JOBS", fb, sizeof fb);
        g_flatten_jobs = (fn > 0 && fn < sizeof fb && atoi(fb) != 0);
        if (g_flatten_jobs)
            log_printf("[cpp_arena] SQUIROLL_FLATTEN_JOBS=1 — cJobThread jobs run INLINE\n");
        char sb[8] = {0};
        DWORD sn = GetEnvironmentVariableA("SQUIROLL_SYNC_WORKERS", sb, sizeof sb);
        g_sync_workers = (sn > 0 && sn < sizeof sb && atoi(sb) != 0);
        if (g_sync_workers)
            log_printf("[cpp_arena] SQUIROLL_SYNC_WORKERS=1 — worker queue drained INLINE (force-sync)\n");
    }

    // Serialises arena_alloc/arena_free across th155's threads. Created
    // before the hooks go live so the very first hooked call is already
    // protected.
    InitializeCriticalSection(&g_lock);

    // MEM_WRITE_WATCH: a snapshot module tracks which pages each frame
    // dirties (GetWriteWatch), so a rollback snapshot copies only what
    // changed, not the whole arena.
    // DETERMINISM: pin the arena to a FIXED base. With VirtualAlloc(nullptr) the OS picks the
    // address, so it varies every run — and every arena-internal pointer (boost::signals2 slot
    // node links, container layout in the render dispatch) varies with it. Anything that orders/
    // hashes by pointer value then behaves differently run-to-run, so the rollback corrupts the
    // dispatch list at a DIFFERENT frame each run (the crash-frame jitter). A fixed base makes the
    // whole arena layout reproducible run-to-run. Fall back to OS-chosen only if the slot is taken.
    g_base = (uint8_t*)VirtualAlloc((void*)0x30000000, ARENA_SIZE,
                                    MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
                                    PAGE_READWRITE);
    if (!g_base) {
        g_base = (uint8_t*)VirtualAlloc(nullptr, ARENA_SIZE,
                                        MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
                                        PAGE_READWRITE);
        log_printf("[cpp_arena] fixed base 0x30000000 unavailable -> OS-chosen %p "
                   "(run-to-run pointer determinism LOST)\n", (void*)g_base);
    }
    if (!g_base) {
        log_printf("[cpp_arena] !! VirtualAlloc(%u) failed — C++ heap stays "
                   "on the CRT heap\n", ARENA_SIZE);
        return;
    }
    log_printf("[cpp_arena] arena base = %p (fixed-base determinism %s)\n",
               (void*)g_base, ((uintptr_t)g_base == 0x30000000) ? "ON" : "OFF");
    g_meta = (Meta*)g_base;
    g_meta->magic      = META_MAGIC;
    g_meta->bump        = (sizeof(Meta) + 15u) & ~15u;  // first SIM block 16-aligned
    g_meta->render_bump = RENDER_BASE;                  // render region grows from here
    g_meta->live_bytes  = 0;
    for (int i = 0; i < NCLS; ++i) g_meta->free_off[i] = 0;

    // Install the FREE hook first: the instant the operator-new hook goes
    // live it hands out arena pointers, and their frees must already be
    // range-routed — otherwise a real _free_base on an arena pointer in the
    // install window would corrupt the CRT heap.
    g_h_free   = safetyhook::create_inline((void*)FREE_BASE,    (void*)hook_free);
    g_h_opnew  = safetyhook::create_inline((void*)OPERATOR_NEW, (void*)hook_op_new);
    g_h_malloc = safetyhook::create_inline((void*)MALLOC_FN,    (void*)hook_malloc);
    g_h_bthreadex = safetyhook::create_inline((void*)BEGINTHREADEX,
                                              (void*)hook_beginthreadex);
    g_h_throw  = safetyhook::create_inline((void*)(0x2FB5DD_R), (void*)throw_log_hook);
    g_h_synclayer = safetyhook::create_inline((void*)(0xE5CD0_R), (void*)synclayer_hook);
    g_h_draw      = safetyhook::create_inline((void*)(0x10A080_R), (void*)draw_eff_hook);
    g_h_runone    = safetyhook::create_inline((void*)(0x2FAD0_R),  (void*)runone_hook);
    g_h_pendframes= safetyhook::create_inline((void*)(0x591D0_R),  (void*)pendframes_hook);
    g_h_numdigit  = safetyhook::create_inline((void*)(0x158A40_R), (void*)numdigit_hook);
    (void)g_h_scripttime; (void)scripttime_hook; (void)g_h_scriptkey; (void)scriptkey_hook;
    (void)det_getasynckeystate; (void)det_timegettime; (void)det_gettickcount;
    // Render-clock determinism is TEST infrastructure (SQUIROLL_DET=1): frame-based clocks
    // freeze pre-match wall-clock, so it's for reproducing/diagnosing the render-dispatch
    // rollback crash, not a default-on fix. It reduces but doesn't eliminate the crash (the
    // residual nondeterminism source is still being chased).
    if (getenv("SQUIROLL_DET"))
    {   // Patch th155's real-time IATs to frame-deterministic stubs (render determinism).
    // Pin th155's entire dynamic address space (heap segments + pools) to deterministic bases.
    // Gated behind SQUIROLL_VABUMP: only useful once the allocation SEQUENCE is deterministic
    // (it isn't yet — the pre-match/render alloc count varies, so the bump just relocates noise).
    if (getenv("SQUIROLL_VABUMP"))
    if (HMODULE nt = GetModuleHandleA("ntdll.dll")) {
        if (void* p = (void*)GetProcAddress(nt, "NtAllocateVirtualMemory")) {
            g_h_ntalloc = safetyhook::create_inline(p, (void*)ntalloc_hook);
            log_printf("[det] NtAllocateVirtualMemory hooked -> deterministic bases from 0x%08X\n",
                       (unsigned)g_va_next);
        }
    }
    g_h_scripttime = safetyhook::create_inline((void*)(0x49450_R),  (void*)scripttime_hook);
    {
        struct { uint32_t iat; void* fn; const char* nm; } pat[] = {
            { 0x3883CC_R, (void*)&det_getasynckeystate, "GetAsyncKeyState" },
            { 0x3884BC_R, (void*)&det_timegettime,      "timeGetTime"      },
            { 0x3880A4_R, (void*)&det_gettickcount,     "GetTickCount"     },
            { 0x3880E0_R, (void*)&det_getlocaltime,     "GetLocalTime(RNG seed)" },
            // QPC NOT hooked: it's pure PROFILING (effect vftable_19 stores it into a
            // perf-stats struct for UpdateLayerPerfCounter), not animation. Our small
            // frame value vs the real pre-hook baseline made elapsed negative -> worse.
            // { 0x3880D8_R, (void*)&det_qpc, "QueryPerfCounter" },
            // { 0x3880DC_R, (void*)&det_qpf, "QueryPerfFreq" },
        };
        (void)det_qpc; (void)det_qpf;
        for (auto& p : pat) {
            void** iat = (void**)(uintptr_t)p.iat;
            DWORD old = 0;
            if (VirtualProtect(iat, sizeof(void*), PAGE_READWRITE, &old)) {
                *iat = p.fn;
                VirtualProtect(iat, sizeof(void*), old, &old);
                log_printf("[det] %s IAT -> frame-deterministic\n", p.nm);
            }
        }
    }
    }   // end if (SQUIROLL_DET)
    g_h_haspend   = safetyhook::create_inline((void*)(0x2FA00_R),  (void*)haspend_hook);
    // numdispose hook NOT installed: proven not the crash (0x64D80 Number-dispose is never
    // called during the rollback). The ~f60-70 abort is the game-loop ScriptAPI's slot-list
    // shared_count release in HasPendingFrame (0x2FA7E) / RunOneFrame, not a Number free.
    // GetFPS (0xEA20) is a display-only Squirrel binding (::GetFPS, used by the
    // gauge_vs.nut FPS overlay, gated by ::config.graphics.fps). It returns a live
    // real-time counter, so the headless re-sim reads a different value than the
    // forward and the FPS TF4_Number's digit buffer (in the snapshotted cpp arena)
    // diverges — the SOLE residual cpp divergence (proven: Dr0 write-watch ->
    // set_number_digit_display -> gauge_vs.nut FPS). Make it deterministic with a
    // direct 6-byte patch (mov eax,60; ret) — safetyhook's trampoline on a 6-byte
    // function broke startup, but it's not used for pacing so a flat patch is safe.
    {
        uint8_t code[6] = { 0xB8, 0x3C, 0x00, 0x00, 0x00, 0xC3 };  // mov eax,60 ; ret
        void* p = (void*)(0xEA20_R);
        DWORD old = 0;
        if (VirtualProtect(p, sizeof code, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(p, code, sizeof code);
            VirtualProtect(p, sizeof code, old, &old);
            log_printf("[getfps] patched GetFPS -> const 60 (FPS overlay determinism)\n");
        }
    }
    // [erase] connection-lifecycle trace — DISABLED again. Confirmed the slot-list count
    // diverges fwd-vs-resim, but the connection keys are blank (anonymous render slots) so
    // it can't name them; the crash is the slot-list shared_count release (HasPendingFrame
    // 0x2FA7E), not an individual erase. Re-enable only to re-trace list-length divergence.
    // g_h_erase  = safetyhook::create_inline((void*)(0x13D80_R),  (void*)erase_log_hook);

    int ok = g_h_free.enabled() + g_h_opnew.enabled();
    g_installed = (ok == 2);
    log_printf("[cpp_arena] install: arena=%p %uMB hooks=%d/2 mallocwatch=%d throwlog=%d\n",
               g_base, ARENA_SIZE / (1024u * 1024u), ok,
               (int)g_h_malloc.enabled(), (int)g_h_throw.enabled());
}

void trace_alloc(uint32_t addr) {
    const MallocRec* best = nullptr;
    for (int k = 0; k < MW_RING; ++k) {
        const MallocRec& r = g_mw[k];
        if (r.ptr && addr >= r.ptr && addr < r.ptr + r.size) {
            // Prefer the tightest enclosing block (the most recent reuse).
            if (!best || r.size < best->size) best = &r;
        }
    }
    if (best) {
        log_printf("[mallocwatch] %08X is inside malloc block [%08X+%X]  "
                   "caller=%08X  rva=%08X\n",
                   addr, best->ptr, best->size, best->caller,
                   (uint32_t)(best->caller - base_address));
    } else {
        log_printf("[mallocwatch] %08X not in malloc ring "
                   "(predates the ring, or not allocated via malloc)\n", addr);
    }
}

void     set_armed(bool on) { g_armed = on; }
bool     is_armed()         { return g_armed; }
void     set_render_pass(bool on) { g_render_pass = on; }
bool     is_excluded_page(uint32_t pg) {           // TF4_Number HUD digit-geometry pages
    return pg < ARENA_PAGES && (g_num_pages[pg >> 5] & (1u << (pg & 31)));
}
uint32_t gameloop_addr() {                         // *0x49AFBC = game-loop render-dispatch ScriptAPI
    return *(uint32_t*)(0x49AFBC_R);
}

// ------------------------------------------------------------ trail determinism --
// B1: the motion-trail (Manbow::TrailLayer) ribbon VERTEX BUFFER is the sole real
// residual cpp divergence + the ~f50 crash. It's built by generate_trail_mesh
// (0x159320, pure CPU/SIMD: U = +96 - i*(+100/(n-1))) only inside the trail DRAW
// (render-side, forward-only), so the headless re-sim never rebuilds it -> stale
// geometry + stale texture refcounts diverge. Fix: rebuild it from the
// (deterministic) circular-buffer positions in the SIM path on BOTH fwd & re-sim,
// writing into the PERSISTENT cpp-arena vertex vector (not the per-frame GPU map),
// so each side snapshots identical geometry.
static void build_one_trail(char* task) {
    char* trail = task + 4;                              // embedded TF4::Trail base
    if (*(uint32_t*)trail != (uint32_t)(0x44C0A4_R))     // sanity: must be a TF4::Trail
        return;
    uint32_t n   = *(uint32_t*)(trail + 128);
    uint32_t cap = *(uint32_t*)(trail + 84);
    if (cap < n) n = cap;
    *(uint32_t*)(trail + 160) = n;                       // active vertex count (draw sets this)
    if (n < 2) return;
    void* vbuf = *(void**)(trail + 168);                // persistent cpp-arena vertex vector data
    if (!vbuf) return;
    *(void**)(trail + 164) = vbuf;                       // redirect output GPU-map -> cpp buffer
    ((void(thiscall*)(int))(0x159320_R))((int)trail);    // generate_trail_mesh
}

// Walk a TF4 TPoolAllocator's used blocks (mirrors *_pool_free_all). idx = the
// dword_4D9F0C index of the free-list head; [+1]=chunk base, [+2]=chunk size off,
// [+3]=element size. Allocated (non-free-list) blocks are live trail tasks.
static void walk_trail_pool(int idx) {
    uint32_t* D = (uint32_t*)(uintptr_t)(0x4D9F0C_R);
    uint32_t chunk = D[idx + 1];
    if (!chunk) return;
    uint32_t coff    = D[idx + 2];
    char*    freeblk = (char*)(uintptr_t)D[idx + 0];
    uint32_t elsz    = D[idx + 3] >= 4u ? D[idx + 3] : *(uint32_t*)(uintptr_t)(0x442A60_R);
    if (elsz & 3) elsz += 4 - (elsz & 3);
    if (elsz < 8) return;
    for (int guard = 0; chunk && guard < 4096; ++guard) {
        char*    end  = (char*)(uintptr_t)(chunk + coff - 8);
        uint32_t next = *(uint32_t*)(uintptr_t)(chunk + coff - 8);
        uint32_t noff = *(uint32_t*)(uintptr_t)(chunk + coff - 4);
        for (char* blk = (char*)(uintptr_t)chunk; blk != end; blk += elsz) {
            if (blk == freeblk) { freeblk = *(char**)freeblk; continue; }
            build_one_trail(blk);
        }
        chunk = next; coff = noff;
    }
}

void rebuild_trail_meshes() {
    static int en = -1;
    if (en == -1) {
        char b[8] = {0};
        DWORD k = GetEnvironmentVariableA("SQUIROLL_TRAIL_REBUILD", b, sizeof b);
        en = (k > 0 && k < sizeof b && atoi(b) != 0) ? 1 : 0;   // default OFF
    }
    if (!en || !g_armed) return;
    // NOTE: the diverging trail is the AnimCtrlTrail (AnimationControllerTrail), NOT the
    // TrailLayer LayerTask/DynLayerTask pools below — and rebuilding the layer tasks
    // corrupts them (crash). Correct target TBD: walk the AnimCtrlTrail pool and rebuild
    // each controller's resource geometry. Left here gated-off pending that retarget.
    (void)build_one_trail;
    walk_trail_pool(2461);
    walk_trail_pool(2473);
}
void     set_resim(bool on) { g_resim = on; }
bool     is_resim()         { return g_resim; }

// DIAGNOSTIC (Phase 1): log + reset the per-advance render/signal alloc counts.
void diag_alloc_counts(int frame, int rb) {
    if (g_diag_render || g_diag_signal)
        log_printf("[allocdiag] f=%d rb=%d render=%u signal=%u signal_offthread=%u\n",
                   frame, rb, g_diag_render, g_diag_signal, g_diag_signal_off);
    g_diag_render = 0;
    g_diag_signal = 0;
    g_diag_signal_off = 0;
}

// Designate the simulation thread — call from the battle/game thread once,
// before snapshot_ring::arm() takes the baseline. From here on only this
// thread's operator new is routed into the arena.
void set_sim_thread(uint32_t tid) {
    if (g_sim_tid == 0 && tid != 0) {
        g_sim_tid = (DWORD)tid;
        log_printf("[cpp_arena] sim thread = %u — arena now thread-gated\n", tid);
    }
}

bool is_sim_thread() {
    return g_sim_tid != 0 && GetCurrentThreadId() == g_sim_tid;
}

// Public raw allocation — hand a block straight out of the arena, bypassing
// the operator-new routing. Used to re-home th155's TF4 Squirrel-instance
// object pool (which would otherwise carve its slabs from the TF4-engine
// mspace, a heap that cannot be rollback-snapshotted because the audio
// thread keeps live decoder state there). Returns nullptr on overflow, so
// the caller can fall back. The block is range-routed back to arena_free
// like any other arena pointer, so a later free is handled.
void* raw_alloc(uint32_t n, uint32_t caller_abs) {
    if (!g_meta) return nullptr;
    // Tag the block with the real th155 call site (passed through by the
    // redirect shim) so attribute() names actual code, not the Netcode.dll
    // shim. See Hdr::link / attribute().
    g_opnew_caller = caller_abs ? caller_abs
                   : (uint32_t)(uintptr_t)_ReturnAddress();
    return arena_alloc(n, is_render_caller(g_opnew_caller) || g_render_depth > 0
                          || (g_sync_workers && g_render_pass));
}

// DIAGNOSTIC: attribute an arena byte offset to the block that owns it and
// log the allocating caller's RVA. Lets a rollback-divergent arena region
// be traced to the exact code — and subsystem (battle / audio / ...) — that
// allocated the object, instead of guessing. Walks the bump arena block by
// block (each block is 1<<cls bytes, contiguous, no gaps).
void attribute(uint32_t off) {
    if (!g_meta) return;
    uint32_t p    = (sizeof(Meta) + 15u) & ~15u;   // first block — see install()
    uint32_t bump = g_meta->bump;
    while (p < bump) {
        Hdr* h = (Hdr*)(g_base + p);
        if (h->cls < (uint32_t)CLS_MIN_SH || h->cls > (uint32_t)CLS_MAX_SH) {
            log_printf("[cpp_attr] off=0x%X: block walk lost at 0x%X\n", off, p);
            return;
        }
        uint32_t blk = 1u << h->cls;
        if (off >= p && off < p + blk) {
            bool live = (h->magic == HDR_MAGIC);
            log_printf("[cpp_attr] off=0x%X -> block@0x%X blk=%u reqsize=%u %s "
                       "caller_rva=%08X\n", off, p, blk, h->reqsize,
                       live ? "LIVE" : "free", live ? h->link : 0u);
            return;
        }
        p += blk;
    }
    log_printf("[cpp_attr] off=0x%X: not in any block (bump=0x%X)\n", off, bump);
}

// DIAGNOSTIC: clear the per-frame arena event trace (call when a frame's
// advance is about to begin recording).
void trace_reset() { g_tr_n = 0; }

// DIAGNOSTIC: at frame `frame`'s capture, if it is the traced frame, save
// the forward run's event sequence the first time and diff a later (re-sim)
// pass against it — the first differing event is where the re-sim deviates.
void trace_check(uint32_t frame, int rb) {
    if (!g_tr_ring_init) {
        for (uint32_t i = 0; i < TR_RING; ++i) g_tr_ring_frame[i] = -1;
        g_tr_ring_init = true;
    }
    uint32_t slot = frame % TR_RING;
    if (!rb) {   // forward (g_resim is already reset to 0 by this point — use rb)
        // Forward: stash this frame's alloc sequence for the later re-sim diff.
        uint32_t n = g_tr_n < TR_SLOT_CAP ? g_tr_n : TR_SLOT_CAP;
        memcpy(g_tr_ring[slot], g_tr, sizeof(TraceEv) * n);
        g_tr_ring_n[slot]     = n;
        g_tr_ring_frame[slot] = (int32_t)frame;
        return;
    }
    // Re-sim: diff against this frame's saved forward sequence; report the
    // first frame whose ALLOC ORDER diverges (the swap onset).
    if (g_tr_ring_frame[slot] != (int32_t)frame || g_tr_div_log <= 0) return;
    uint32_t rn  = g_tr_n < TR_SLOT_CAP ? g_tr_n : TR_SLOT_CAP;
    uint32_t sn  = g_tr_ring_n[slot];
    uint32_t lim = rn < sn ? rn : sn;
    uint32_t i = 0;
    for (; i < lim; ++i) {
        const TraceEv& a = g_tr_ring[slot][i];
        const TraceEv& b = g_tr[i];
        if (a.op != b.op || a.size != b.size || a.caller != b.caller || a.off != b.off)
            break;
    }
    if (i >= lim && rn == sn) return;     // identical — nothing to report
    --g_tr_div_log;
    log_printf("[cpptrace] f=%u FIRST DIVERGENT ALLOC @ event #%u (fwd %u events, "
               "resim %u)\n", frame, i, sn, rn);
    uint32_t lo = i >= 3 ? i - 3 : 0;
    uint32_t hi = (i + 4 < lim) ? i + 4 : lim;
    for (uint32_t k = lo; k < hi; ++k) {
        const TraceEv& a = g_tr_ring[slot][k];
        const TraceEv& b = g_tr[k];
        log_printf("[cpptrace]   #%u  FWD %s sz=%u caller=%08X off=%X  vs  RESIM %s "
                   "sz=%u caller=%08X off=%X %s\n", k,
                   a.op == 1 ? "alloc" : "free ", a.size, a.caller, a.off,
                   b.op == 1 ? "alloc" : "free ", b.size, b.caller, b.off,
                   k == i ? "<-- FIRST DIFF" : "");
    }
}
uint8_t* base()      { return g_base; }
uint32_t used()      { return g_meta ? g_meta->bump : 0; }
uint32_t capacity()  { return ARENA_SIZE; }
size_t   live_bytes(){ return g_meta ? g_meta->live_bytes : 0; }

uint32_t save(uint8_t* out, uint32_t cap) {
    if (!g_meta) return 0;
    uint32_t n = g_meta->bump;
    if (n > cap) {
        log_printf("[cpp_arena] !! save OVERFLOW used=%u > cap=%u\n", n, cap);
        return 0;
    }
    memcpy(out, g_base, n);
    return n;
}

void load(const uint8_t* blob, uint32_t len) {
    if (!g_base || len < sizeof(Meta) || len > ARENA_SIZE) return;
    if (((const Meta*)blob)->magic != META_MAGIC) {
        log_printf("[cpp_arena] load: bad magic\n");
        return;
    }
    memcpy(g_base, blob, len);
}

} // namespace cpp_arena
