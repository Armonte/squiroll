#if __INTELLISENSE__
#undef _HAS_CXX20
#define _HAS_CXX20 0
#endif

#include <windows.h>
#include <dxgi.h>
#include <float.h>

#include "log.h"
#include "patch_utils.h"
#include "util.h"
#include "config.h"
#include "gekko_bridge.h"
#include "cpp_arena.h"
#include "render_arena.h"

// Superluminal markers for debugging frametime spikes
#define PROFILING 0

typedef void stdcall window_update_frame_t();
typedef bool stdcall window_render_t();
typedef void fastcall run_update_list_t(void*);

#define game_loop_call_addr (0x1A22D_R)
#define wndproc_param_addr (0x23944_R)
#define set_window_mode_param_addr (0xEAB6_R)
#define no_gdi_compat_patch_addr (0xD687_R)
#define no_input_thread_patch_addr (0x3175C_R)

#define main_hwnd (*(HWND*)0x4DAD0C_R)
#define window_vsync ((bool*)0x49AF01_R)
#define window_present_interval ((bool*)0x4DAD2D_R)
#define dxgi_swapchain_desc ((DXGI_SWAP_CHAIN_DESC*)0x4DAE4C_R)
#define dxgi_swapchain ((IDXGISwapChain**)0x4DAEA0_R)
#define input_update_list ((void**)0x49AF8C_R)

#define window_update_frame ((window_update_frame_t*)0xE1A0_R)
#define window_render ((window_render_t*)0xE330_R)
#define WndProc_orig ((WNDPROC)0x23A70_R)
#define run_update_list ((run_update_list_t*)0x2FAD0_R)

#if PROFILING
typedef struct {
    int64_t SuppressTailCall[3];
} PerformanceAPI_SuppressTailCallOptimization;
typedef void (PerformanceAPI_BeginEvent_Func)(const char* inID, const char* inData, uint32_t inColor);
typedef PerformanceAPI_SuppressTailCallOptimization (PerformanceAPI_EndEvent_Func)();
typedef struct {
    uintptr_t pad0[2];
    PerformanceAPI_BeginEvent_Func* BeginEvent;
    uintptr_t padC[3];
    PerformanceAPI_EndEvent_Func* EndEvent;
    uintptr_t pad1C[4];
} PerformanceAPI_Functions;
typedef int (*PerformanceAPI_GetAPI_Func)(int inVersion, PerformanceAPI_Functions* outFunctions);
#endif

static void set_fullscreen(BOOL fullscreen) {
    if (dxgi_swapchain_desc->Windowed == fullscreen) {
        dxgi_swapchain_desc->Windowed = !fullscreen;
        HRESULT res = (*dxgi_swapchain)->SetFullscreenState(fullscreen, nullptr);
        if (FAILED(res)) {
            log_printf("SetFullscreenState failed: 0x%X\n", res);
        }

        SetForegroundWindow(main_hwnd);
    }
}

static void cdecl set_window_mode_custom(int, int, bool fullscreen, bool vsync) {
    set_fullscreen(fullscreen);
    *window_vsync = vsync;
    *window_present_interval = vsync;
}

static bool fullscreen_queued = false;
static bool exit_requested = false;

// Ends the frame loop cleanly (loop exits -> WM_DESTROY). Called when the
// remote peer drops (GekkoPlayerDisconnected): GekkoNet otherwise synthesizes
// the dead peer's inputs and the survivor plays on solo forever, hanging the
// window (and holding Netcode.dll open across a redeploy). v1 = close; a
// shipping build should instead unwind to the main menu with a "peer left"
// notice — see task #29.
void request_shutdown() { exit_requested = true; }
// GetFPS() (th155 0xEA21, patched below to read this) feeds a script-computed
// sim dt = 1/GetFPS(). It updates once/sec from the real frame count, so it
// drifts across a rollback burst -> a frame's forward pass and its re-sim read
// different fps -> different dt -> desync (round-transition sq 0x241FC8D0).
// advance_one_frame freezes it to 60 for the deterministic sim via these.
uint32_t current_fps = 60;   // what GetFPS() returns — pinned to 60 for determinism
uint32_t measured_fps = 0;   // real measured rate, for display/diagnostics only
uint32_t sim_get_fps() { return current_fps; }
void     sim_set_fps(uint32_t v) { current_fps = v; }
uint32_t render_fps()  { return measured_fps; }

// Monotonic real-frame counter, bumped once per better_game_loop iteration
// (i.e. once per real displayed frame, in EVERY branch incl. the vanilla
// transition). Used by the menu-mash keyboard hook to toggle the confirm key
// per real frame — GetTickCount can't be used (SQUIROLL_DET hooks it) and a
// per-poll-call counter is unreliable (the poll fires a variable number of
// times per frame).
uint32_t g_real_frame = 0;
uint32_t sim_real_frame() { return g_real_frame; }

void stdcall better_game_loop() {
    // This is th155's game/simulation thread. Designate it to cpp_arena now
    // — far earlier than gekko_bridge::init — so the arena is thread-gated
    // before menus, vs.Initialize or audio do any allocating. From here on
    // ONLY this thread's operator new enters the rollback snapshot.
    cpp_arena::set_sim_thread(GetCurrentThreadId());

    // Disable DXGI's Alt+Enter handler because it's unreliable sometimes (handled in custom WndProc instead)
    size_t enter_held_frames = 0;
    IDXGIFactory* dxgi_factory = nullptr;
    (*dxgi_swapchain)->GetParent(__uuidof(IDXGIFactory), (void**)&dxgi_factory);
    dxgi_factory->MakeWindowAssociation(main_hwnd, DXGI_MWA_NO_ALT_ENTER);

    // Make the scheduler friendlier
    SetThreadPriority(CurrentThreadPseudoHandle(), THREAD_PRIORITY_HIGHEST);
    timeBeginPeriod(1);

    // The vanilla game loop does this for some reason, so this needs to do the same to not desync replays
    unsigned int fpu_state = 0;
    _clearfp();
    _controlfp_s(&fpu_state, _RC_NEAR | _PC_64, _MCW_RC | _MCW_PC);

#if PROFILING
    PerformanceAPI_Functions perf_api;
    HMODULE perf_dll = LoadLibraryW(L"C:\\Program Files\\Superluminal\\Performance\\API\\dll\\x86\\PerformanceAPI.dll");
    ((PerformanceAPI_GetAPI_Func)GetProcAddress(perf_dll, "PerformanceAPI_GetAPI"))(0x30000, &perf_api);
#endif

    // TODO: Investigate how precise this is without CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
    WaitableTimer timer;
    timer.initialize();
    uint64_t qpc_next_fps_update = current_qpc() + qpc_second_frequency;
    uint32_t frames_this_sec = 0;
    int64_t leniency = get_timer_leniency() * 10000;

    while (expect(!exit_requested, true)) {
        ++g_real_frame;   // one per real displayed frame (all branches)
#if PROFILING
        perf_api.BeginEvent("Frame", nullptr, 0xFFFFFFFF);
#endif

        uint64_t qpc_target = current_qpc() + qpc_frame_frequency;
        timer.set(166667 - leniency);

        // tick() pumps the UDP poll + session events whenever a session
        // exists. Pre-SessionStarted it does only that, so the vanilla
        // update path still drives the menu UI through the sync handshake.
        // Post-SessionStarted, tick() owns the frame loop (calls
        // advance_one_frame from gekko Advance events) and we skip the
        // vanilla run_update_list to avoid double-stepping the engine.
        if (gekko_bridge::is_session_started()) {
            // tick() IS the frame loop now — it steps the engine via
            // advance_one_frame() for every gekko Advance event (and
            // ticks the background ScriptAPI itself). We must NOT also
            // call window_update_frame() (== update_logic, 0xE1A0):
            // that runs another full RunOneFrame, double-stepping the
            // engine every real frame. The forward pass would advance
            // battle.count / the round timer twice per frame while a
            // rollback re-sim (entirely inside tick()) steps once — the
            // counters desync and the round machine races. Render only.
            // turbo_ticks() is 1 normally; >1 when a solo stress session
            // is fast-forwarding. Extra tick()s advance more logical
            // frames without an extra render — render stays once per
            // real frame so the window keeps repainting at full rate.
            for (int i = 0, n = gekko_bridge::turbo_ticks(); i < n; ++i) {
                gekko_bridge::tick();
            }
            frames_this_sec += window_render();
        } else if (gekko_bridge::is_active()) {
            // Dual handshake hold: the GekkoGameSession was created at
            // Round_Fight and its UDP handshake is in flight. Pump
            // tick() so the handshake completes and SessionStarted can
            // fire, but DO NOT advance the engine — the game holds on
            // fight-frame-0 until both peers are connected, so gekko
            // frame 0 is the identical Round_Fight state on both sides.
            // (Solo never reaches here — init_solo() sets is_active and
            // is_session_started together, so it goes straight to the
            // branch above.)
            gekko_bridge::hold_vanilla_feed();   // keep feeding the peer's delay netcode
            gekko_bridge::tick();
            frames_this_sec += window_render();
        } else if (gekko_bridge::is_holding_transition()) {
            // Round-end TRANSITION HOLD: we latched first. Do NOT run the
            // vanilla update (its delay-netcode traffic derails the still-armed
            // peer); keep polling the gekko session (retransmits + acks) and
            // rendering until the remote has reached the latch frame too.
            gekko_bridge::hold_poll();
            frames_this_sec += window_render();
        } else {
            // Pre-arm: the menu and the round-start intro run here under
            // the vanilla loop. pre_arm_poll() arms the gekko session
            // the instant battle.state hits Round_Fight, so gekko never
            // rolls back the intro.
            run_update_list(*input_update_list);
            window_update_frame();
            gekko_bridge::pre_arm_poll();
            // NOTE: Not handling "SkipRender" because that doesn't seem to be used in AoCF
            frames_this_sec += window_render();
        }

#if PROFILING
        perf_api.EndEvent();
#endif

        // Drain the log buffers once per frame. The log file + console
        // are fully buffered (see open_log_file); flushing here keeps
        // them current without the per-line disk/console writes that
        // were stalling the frame.
        log_flush();

        uint64_t now = current_qpc();
        if (now < qpc_target) {
            timer.wait();
            now = current_qpc();
            if (now > qpc_target) {
                log_printf("Scheduler overshot frame deadline by %llu " SYNC_UNIT_STR "\n", qpc_to_sync_time(now - qpc_target));
            }
            else {
                while (now < qpc_target) {
                    _mm_pause();
                    now = current_qpc();
                }
            }
        }

        if (expect_chance(now >= qpc_next_fps_update, true, FRAC_SECONDS_PER_FRAME)) {
            qpc_next_fps_update = now + qpc_second_frequency;
            measured_fps = frames_this_sec;
            // GetFPS() (which scripts read, incl. a per-frame dt = 1/GetFPS the
            // battle HUD stores in SIM-checksummed state) must be deterministic:
            // the measured value jitters 59/60/61 with real frame timing, so a
            // rollback re-sim of a frame reads a different fps than its forward
            // pass -> desync (round transition, sq 0x241FC8D0). The game is a
            // fixed 60 Hz sim, so pin GetFPS to 60. measured_fps keeps the real
            // value for anything that wants the true rate.
            current_fps = 60;
            frames_this_sec = 0;
        }

        if (expect(fullscreen_queued, false)) {
            fullscreen_queued = false;
            set_fullscreen(dxgi_swapchain_desc->Windowed);
        }
    }
    // [r2diag] a silent process exit (no crash, no watchdog) = the frame loop
    // returned; name why so a window close / quit message is distinguishable.
    log_printf("[loop] frame loop exited: exit_requested=%d real_frame=%u\n", (int)exit_requested, (unsigned)g_real_frame);
    log_flush();


    _controlfp_s(&fpu_state, _CW_DEFAULT, 0xFFFFFFFF);
    timeEndPeriod(1);
    SendMessageA(main_hwnd, WM_DESTROY, 0, 0);
}

LRESULT stdcall WndProc_custom(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    if (Msg == WM_SYSKEYDOWN && (HIWORD(lParam) & KF_ALTDOWN) && wParam == VK_RETURN) {
        fullscreen_queued = true;
        return 0;
    }
    return WndProc_orig(hWnd, Msg, wParam, lParam);
}

void init_better_game_loop() {
    hotpatch_rel32(game_loop_call_addr, better_game_loop);

    mem_write(0xEA21_R, &current_fps);
    mem_write(0xE0A7_R, &exit_requested);
    mem_write(0xE192_R, &exit_requested);

    mem_write(wndproc_param_addr, &WndProc_custom);
    mem_write(set_window_mode_param_addr, &set_window_mode_custom);
    mem_write(no_gdi_compat_patch_addr, PATCH_BYTES<0x00>); // Not used at all and it's probably better to turn it off
    mem_write(no_input_thread_patch_addr, NOP_BYTES(5));
}
