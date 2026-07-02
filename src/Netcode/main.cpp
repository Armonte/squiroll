#if __INTELLISENSE__
#undef _HAS_CXX20
#define _HAS_CXX20 0
#endif

#include <string.h>
#include <limits.h>
#include <string>
#include <vector>
#include <cwchar>

#include "netcode.h"
#include "util.h"
#include "alloc_man.h"
#include "patch_utils.h"
#include "log.h"
#include "file_replacement.h"
#include "config.h"
#include "lobby.h"
#include "better_game_loop.h"
#include "discord.h"
#include "RSACache.h"
#include "live_actors.h"
#include "focus_input.h"
#include "crash_handler.h"
#include "sq_arena.h"
#include "cpp_arena.h"
#include "bullet_arena.h"
#include "tf4_arena.h"
#include "sync_pin.h"
#include "input_hist.h"
#include "tf4_pool.h"
#include "sq_trace.h"
#include "actor2d_log.h"
#include "sqfun_log.h"
#include "input_command_log.h"
#include "input_global_sync.h"
#include "cl_iter_guard.h"
#include "eft_freer_log.h"
#include "tree_walker_guard.h"

#include <shared.h>

using namespace std::literals::string_literals;
using namespace std::literals::string_view_literals;

#if !MINGW_COMPAT
#pragma comment (lib, "Ws2_32.lib")
#endif

extern "C" IMAGE_DOS_HEADER __ImageBase;

const uintptr_t base_address = (uintptr_t)GetModuleHandleA(NULL);
uintptr_t libact_base_address = 0;

uint64_t qpc_second_frequency;
uint64_t qpc_frame_frequency;
uint64_t qpc_milli_frequency;
uint64_t qpc_micro_frequency;

static const auto patch_se_upnp = [](void* base_address) {
#if ALLOCATION_PATCH_TYPE == PATCH_ALL_ALLOCS
    hotpatch_jump(based_pointer(base_address, 0x1C4BB), my_malloc);
    hotpatch_jump(based_pointer(base_address, 0x21229), my_calloc);
    hotpatch_jump(based_pointer(base_address, 0x1C509), my_realloc);
    hotpatch_jump(based_pointer(base_address, 0x215AD), my_free);
    hotpatch_jump(based_pointer(base_address, 0x2D3BF), my_recalloc);
    hotpatch_jump(based_pointer(base_address, 0x32413), my_msize);
#endif
};

static auto patch_se_information = [](void* base_address) {
#if ALLOCATION_PATCH_TYPE == PATCH_ALL_ALLOCS
    hotpatch_jump(based_pointer(base_address, 0x1B853), my_malloc);
    hotpatch_jump(based_pointer(base_address, 0x1BBE9), my_calloc);
    hotpatch_jump(based_pointer(base_address, 0x1B8A1), my_realloc);
    hotpatch_jump(based_pointer(base_address, 0x1BFAE), my_free);
    hotpatch_jump(based_pointer(base_address, 0x25FF0), my_recalloc);
    hotpatch_jump(based_pointer(base_address, 0x2B406), my_msize);
#endif

    // Timeout for update to prevent lag on main menu
    // Currently removed completely via squirrel instead
    //mem_write(based_pointer(base_address, 0xED3B), 10000);
};

static auto patch_se_trust = [](void* base_address) {
#if ALLOCATION_PATCH_TYPE == PATCH_ALL_ALLOCS
    hotpatch_jump(based_pointer(base_address, 0x5BA7), my_malloc);
    hotpatch_jump(based_pointer(base_address, 0x5C92), my_calloc);
    hotpatch_jump(based_pointer(base_address, 0x936E), my_realloc);
    hotpatch_jump(based_pointer(base_address, 0x5B6D), my_free);
    hotpatch_jump(based_pointer(base_address, 0x78AB), my_recalloc);
    hotpatch_jump(based_pointer(base_address, 0x933B), my_msize);
#endif

    /*
    static constexpr const uint8_t data[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,   // MOV EAX, 1
        0xC3,                           // RET
        0xCC                            // INT3
    };
    mem_write(based_pointer(base_address, 0x13C0), data);
    */
};

typedef void* thisfastcall act_script_plugin_load_t(
    void* self,
    thisfastcall_edx(int dummy_edx, )
    const char* plugin_path
);

void* thisfastcall patch_act_script_plugin(
    void* self,
    thisfastcall_edx(int dummy_edx,)
    const char* plugin_path
) {
    void* base_address = based_pointer<act_script_plugin_load_t>(libact_base_address, 0x1CC60)(
        self,
        thisfastcall_edx(dummy_edx,)
        plugin_path
    );

    if (base_address) {
        log_printf("Applying patches for \"%s\" at %p from ACT\n", plugin_path, base_address);
        if (!strcmp(plugin_path, "data/plugin/se_lobby.dll")) {
            patch_se_lobby(base_address);
            goto plugin_load_end;
        }
        if constexpr (!IS_CONSTEXPR(patch_se_upnp(nullptr))) {
            if (!strcmp(plugin_path, "data/plugin/se_upnp.dll")) {
                patch_se_upnp(base_address);
                goto plugin_load_end;
            }
        }
        if constexpr (!IS_CONSTEXPR(patch_se_information(nullptr))) {
            if (!strcmp(plugin_path, "data/plugin/se_information.dll")) {
                patch_se_information(base_address);
                goto plugin_load_end;
            }
        }
        if constexpr (!IS_CONSTEXPR(patch_se_trust(nullptr))) {
            if (!strcmp(plugin_path, "data/plugin/se_trust.dll")) {
                patch_se_trust(base_address);
                goto plugin_load_end;
            }
        }
    }

plugin_load_end:
    return base_address;
}

static void patch_se_libact(void* base_address) {
    libact_base_address = (uintptr_t)base_address;

    // (The old alloc_man my_malloc/my_free hotpatches lived here. Removed:
    // the rollback heap snapshot is sq_arena — which hooks the Squirrel
    // allocator wrappers and captures the whole Squirrel subsystem — plus
    // cpp_arena for std::list nodes. alloc_man/sq_heap was the superseded
    // tracking approach and is no longer part of the rollback path.)

    hotpatch_rel32(based_pointer(base_address, 0x15F7C), patch_act_script_plugin);
};

#define load_plugin_from_pak_addr (0x12DDD0_R)

void* thisfastcall patch_exe_script_plugin(
    void* self,
    thisfastcall_edx(int dummy_edx,)
    const char* plugin_path
) {
    void* base_address = ((act_script_plugin_load_t*)load_plugin_from_pak_addr)(
        self,
        thisfastcall_edx(dummy_edx,)
        plugin_path
    );

    if (base_address) {
        log_printf("Applying patches for \"%s\" at %p from EXE\n", plugin_path, base_address);
        if (!strcmp(plugin_path, "data/plugin/se_libact.dll")) {
            patch_se_libact(base_address);
            goto plugin_load_end;
        }
        if (!strcmp(plugin_path, "data/plugin/se_lobby.dll")) {
            patch_se_lobby(base_address);
            goto plugin_load_end;
        }
        if constexpr (!IS_CONSTEXPR(patch_se_upnp(nullptr))) {
            if (!strcmp(plugin_path, "data/plugin/se_upnp.dll")) {
                patch_se_upnp(base_address);
                goto plugin_load_end;
            }
        }
        if constexpr (!IS_CONSTEXPR(patch_se_information(nullptr))) {
            if (!strcmp(plugin_path, "data/plugin/se_information.dll")) {
                patch_se_information(base_address);
                goto plugin_load_end;
            }
        }
        if constexpr (!IS_CONSTEXPR(patch_se_trust(nullptr))) {
            if (!strcmp(plugin_path, "data/plugin/se_trust.dll")) {
                patch_se_trust(base_address);
                goto plugin_load_end;
            }
        }
    }
    else {
        if (!strcmp(plugin_path, "data/plugin\\data/plugin/se_libact.dll.dll")) {
            base_address = &__ImageBase;
        }
    }

plugin_load_end:
    return base_address;
}

#define entrypoint_base_addr (0x2E1B8C_R)
#define D3DX11CreateShaderResourceViewFromMemory_import_addr (0x388534_R)

#define patch_act_script_plugin_hook_addr (0x127ADC_R)

#define createmutex_patch_addr (0x01DC61_R)

#define IsProcessorFeaturePresent_import_addr (0x388308_R)

static inline void disable_original_game_logging() {
    // Disable regular printf
    hotpatch_ret(0x25270_R, 0);
    // Skip libpng warning fprintf calls
    mem_write(0x13BBD1_R, PATCH_BYTES<0x09>);
}

static BOOL __stdcall IsProcessorFeaturePresent_hook(DWORD ProcessorFeature) {
    if (ProcessorFeature == PF_FASTFAIL_AVAILABLE)
        return FALSE;
    return IsProcessorFeaturePresent(ProcessorFeature);
}

static void cdecl parse_command_line(const char* str) {
    //log_printf("Command line: \"%s\"\n", str);
    switch (str[0]) {
        case 'w': case 'W':
            log_printf("Watch from boot \"%s\"\n", str + 1);
            break;
        case 'c': case 'C':
            log_printf("Connect from boot \"%s\"\n", str + 1);
            break;
    }
}

//static char* launch_str = "th155r.exe";

// Initialization code shared by th155r and thcrap use
// Executes before the start of the process
bool common_init(
#if !DISABLE_ALL_LOGGING_FOR_BUILD
    LogType log_type
#endif
) {
    init_config_file();

    if (expect(GAME_VERSION != 1211, false)) {
        MessageBoxA(NULL, "Your game appears to be too old. Please update it to v1.21b.\nIf you already updated it, delete config.ini in the game directory.", "squiroll", MB_ICONERROR);
        return false;
    }

#if !DISABLE_ALL_LOGGING_FOR_BUILD
    if (log_type != NO_LOGGING) {
        enable_debug_console(log_type == LOG_TO_PARENT_CONSOLE);
        patch_throw_logs();
        // Tee stdout/stderr into squiroll.log without redirecting them, so
        // the console keeps live output AND the file survives a crash.
        // Dual-instance test runs use SQUIROLL_LOG_NAME for a full
        // filename override (e.g. aocf_net_p1.log), or fall back to
        // SQUIROLL_LOG_SUFFIX which produces squiroll_<suffix>.log.
        char log_name[64] = "squiroll.log";
        char override_buf[64] = {0};
        DWORD n = GetEnvironmentVariableA("SQUIROLL_LOG_NAME", override_buf, sizeof(override_buf));
        if (n > 0 && n < sizeof(override_buf)) {
            snprintf(log_name, sizeof(log_name), "%s", override_buf);
        } else {
            char suffix[24] = {0};
            n = GetEnvironmentVariableA("SQUIROLL_LOG_SUFFIX", suffix, sizeof(suffix));
            if (n > 0 && n < sizeof(suffix)) {
                snprintf(log_name, sizeof(log_name), "squiroll_%s.log", suffix);
            }
        }
        open_log_file(log_name);
        log_printf  = tee_printf;
        log_fprintf = tee_fprintf;
    }
    else {
        log_printf = printf_dummy;
        log_fprintf = fprintf_dummy;
#if !ALWAYS_DISABLE_ORIGINAL_GAME_LOGGING
        // Disable the original game's printf use
        // when logging is disabled
        disable_original_game_logging();
#endif
    }
#endif

#if !DISABLE_ALL_LOGGING_FOR_BUILD || ALWAYS_DISABLE_ORIGINAL_GAME_LOGGING
    disable_original_game_logging();
#endif

    // VEH crash logger — install before any of our patches/hooks so a
    // fault anywhere lands a module+RVA report in aocf_crash.log.
    crash_handler::install();

    // IAT-patch th155's VirtualAlloc so the two engine-private "TF4 mspace"
    // dlmalloc pools (allocated by tf4_mspace_create during engine init) get
    // MEM_WRITE_WATCH + fixed bases and are recorded for rollback snapshotting.
    // common_init runs at th155's entrypoint hook, BEFORE the original
    // entrypoint (and hence engine init / tf4_mspace_create) is tail-called —
    // so this is early enough to intercept the pool VirtualAllocs. Verified at
    // runtime by the "[tf4_arena] pool ... intercepted" log lines and the
    // absence of a "TOO LATE" line.
    tf4_arena::early_install();
    sync_pin::install();   // must precede engine init: registers every lock created inside the arenas

    // Redirect th155's Squirrel-instance object pool to allocate its slabs
    // from cpp_arena (rollback-snapshotted) instead of the TF4-engine mspace
    // (which can't be — the audio thread keeps live decoder state there).
    // Must run before any battle Squirrel instance is created.
    tf4_pool::install();

    hotpatch_rel32(0x1DC5A_R, parse_command_line);

    // Turn off scroll lock to simplify static management for the toggle func
    SetScrollLockState(false);

    // Route the Squirrel VM heap into a fixed arena (sq_arena) — the basis
    // for the giuroll-style raw memory snapshot of the VM state. The three
    // Squirrel allocator wrappers (sq_malloc/realloc/free_base) are hooked;
    // a caller whose return address is in the Squirrel VM code block
    // [0x17FDB0, 0x1A6000) is routed to the arena, everyone else to the
    // real CRT heap. Must run before the Squirrel VM is created (sq_open) —
    // common_init is well before that. (Supersedes the old alloc_man
    // patch_allocman() tracking approach.)
    sq_arena::install(0x17FDB0_R, 0x1A6000_R);

    // Route every std::list node (Act::ScriptAPI task lists, Actor2DProcGroup,
    // ...) into a fixed arena (cpp_arena) so the per-frame list churn is part
    // of the rollback snapshot. Hooks _Buynode0 (node alloc) + _free_base
    // (free). Must run before the battle's lists are populated — common_init
    // is well before vs.Initialize.
    cpp_arena::install();
    // Arm cpp_arena for the whole process lifetime so EVERY th155 C++ heap
    // allocation — including the battle objects (characters, animation
    // controllers, their std::vector buffers) created during vs.Initialize —
    // lands in the rollback-captured arena. A battle object left on the
    // un-captured real Win32 heap double-frees on a rollback re-sim.
    cpp_arena::set_armed(true);

    // Route th155's Bullet physics heap into a captured arena. Bullet's two
    // base allocator fn-pointers (0x498D44 alloc / 0x498D48 free) are
    // overwritten; the aligned wrappers delegate to them, so the whole
    // physics heap — collision world, broadphase, shapes, and the per-sprite
    // ActorCollisionData hitbox shapes — lands in the snapshot. Must run
    // before th155 creates any Bullet object; common_init is well before.
    bullet_arena::install();

    // Hook input_history_u16__append so input_hist can discover the per-player
    // input-history objects (uncaptured th155-heap state — the cpp_arena
    // rollback divergence). Must be live before the battle starts appending.
    input_hist::install();

    // Diagnostic: catch the dual-rollback crash (SQInstance member-get on
    // a class whose _members table is NULL) at its source, logging the
    // class and member name instead of faulting deeper in SQTable__Get.
    sq_trace::install();

    // Allow launching multiple instances of the game
    mem_write(createmutex_patch_addr, PATCH_BYTES<0x68, 0x00, 0x00, 0x00, 0x00>); //mutex patch

    patch_netplay();

    // Hook Actor2DManager::CreateActor2D* + Actor2D::Release so we have a
    // live registry of every ManbowActor2D* the moment one comes into
    // existence. Required by the rollback save/load path (gekko_bridge).
    // Must run before any Squirrel-driven actor creation.
    live_actors::install();

    // DIAGNOSTIC: per-call dump of the count_this vs count_child comparison
    // in Manbow::Actor2D::UpdateChildMatrices — pins which Actor2D produces
    // the f=15 rollback divergence by flipping the createProxy/setAabb
    // branch. Installs unconditionally; the hook self-gates to a tiny frame
    // window so it's silent in production play outside that window.
    actor2d_log::install();

    // DIAGNOSTIC: per-call dump of the SQObject targeted by the
    // Sqrat::Function callback that Actor2DGroup::Update_mask1 fires between
    // its Update and StepMovement passes. Same in-window gating as a2dlog.
    // The divergent f=15 re-sim writes per-actor velocities BETWEEN our
    // a2dstep "noUCM(v=0)" entries (where vx=0) and "UCM" (where vx=11.5);
    // a script function called from this site is the prime suspect.
    sqfun_log::install();

    // DIAGNOSTIC: per-call dump of Manbow::InputCommand::Update's input
    // source vtable. This identified Manbow::InputGlobal/InputMulti as
    // the source (see input_command_log.cpp comments) and is now disabled
    // because input_global_sync hooks the SAME function and only one
    // SafetyHookInline can own a given entry — the fix takes priority.
    // Re-enable temporarily by commenting out the sync hook below if you
    // need to re-examine the input source identity.
    // input_command_log::install();

    // FIX: capture-and-replay the polled input state at InputCommand::Update
    // entry. The 32-byte buffer at *(InputCommand+0)+4 is what the function
    // body reads via vtable[4]; capturing it on forward and restoring on
    // every re-sim makes InputCommand's per-frame ring write deterministic
    // regardless of who polls the underlying device.
    input_global_sync::install();

    // DEFENSIVE: short-circuit concurrent_list_iter_step when its iter
    // position field holds one of our arena bases (cpp/sq/bt). That
    // value comes from a still-unattributed corruptor, and reaches
    // walk_visit where it faults on the payload deref. The clguard in
    // crash_handler absorbs the VEH-level AV; this hook prevents it
    // from happening at all by skipping the walk upstream.
    cl_iter_guard::install();

    // DIAGNOSTIC: hook the cEftGroup destructor (sub_ECB00) and the prune
    // walk (sub_EC130) to identify any code path that destructs a group
    // while it is still in Ew::sEffect's live-groups vector at
    // sEffect+0xEC/0xF0. That is the buggy-freer pattern behind the
    // "crash on hit" UAF — the universal NULL-skip in crash_handler
    // currently absorbs the consequence but doesn't tell us the source.
    // Hits land in the Netcode log as "[eftfreer] !!! BUGGY FREER".
    eft_freer_log::install();

    // FIX: bound std__map_string_int__lower_bound walk to kMaxSteps so a
    // corrupted boost::signals2 grouped_list tree can't hang the game.
    // Without this, a NULL or non-canonical child pointer makes the loop
    // fault every iteration; crash_handler's universal NULL-skip absorbs
    // the AV but never updates the node register, so the loop spins
    // forever — observed as a freeze-on-hit during rollback re-sim.
    tree_walker_guard::install();

    // Two-instance local testing: gate XInput reads by which window has
    // focus, so the same controller drives whichever player owns the
    // foreground process. DInput already does this natively via the
    // game's DISCL_FOREGROUND cooperative-level setup.
    focus_input::install();

    LARGE_INTEGERX qpc_freq;
    QueryPerformanceFrequency(&qpc_freq);

    qpc_second_frequency = (uint64_t)qpc_freq;
    qpc_frame_frequency = (uint64_t)qpc_freq / 60; // frames per second
    qpc_milli_frequency = (uint64_t)qpc_freq / 1000; // millisecond per second
    qpc_micro_frequency = (uint64_t)qpc_freq / 1000000; // microsecond per second

    patch_sockets();

    hotpatch_rel32(patch_act_script_plugin_hook_addr, patch_exe_script_plugin);

    //mem_write(0x1DEAD_R, INFINITE_LOOP_BYTES); // Replaces a TEST ECX, ECX

    if (get_cache_rsa_enabled())patch_archive_parsing();

    // Disable fastfail to allow exception handlers to catch more crashes
    hotpatch_import(IsProcessorFeaturePresent_import_addr, IsProcessorFeaturePresent_hook);

    init_better_game_loop();

#if ENABLE_DISCORD_INTEGRATION
    int8_t discord_state = get_discord_enabled();
    if (TST_CONFIG_MAYBE(discord_state)) {
        // TODO: Add something to test if discord is actually present
        set_discord_enabled(true);
        discord_state = true;
    }
    if (TST_CONFIG_ENABLED(discord_state)) {
        DISCORD_ENABLED = true;
        discord_rpc_start();
    }
#endif

    // Force Manbow::Texture::GetBase64 to not load from pak files
    mem_write(0x5B9A1_R, PATCH_BYTES<0x6A, 0x00>);
    mem_write(0x5B9A3_R, NOP_BYTES(4));

    return true;
}

static void yes_tampering() {
    static constexpr uintptr_t tamper_patch_addrs[] = {
        0x12E820,
        //0x130630,
        0x132AF0
    };

    uintptr_t base = base_address;
    nounroll for (size_t i = 0; i < countof(tamper_patch_addrs); ++i) {
        hotpatch_ret(based_pointer(base, tamper_patch_addrs[i]), 0);
    }
}

// Note: the WinMain mutex check at 0x41DC81 was previously patched here
// via JNZ -> JMP. That's redundant — squiroll already neutralizes the
// CreateMutexA by replacing the mutex-name push with `push 0` at
// `createmutex_patch_addr` (line 299), turning the named mutex into an
// anonymous one so a second instance never collides.

typedef BOOL cdecl globalconfig_get_boolean_t(const char* key, const BOOL default_value);
typedef const char* cdecl runconfig_runcfg_fn_get_t();
typedef const char* cdecl runconfig_thcrap_dir_get_t();

#if !DISABLE_ALL_LOGGING_FOR_BUILD
static bool enable_thcrap_console = false;
#endif

extern "C" {
    // FUNCTION REQUIRED FOR THE LIBRARY
    // th155r init function
    dll_export int stdcall netcode_init(InitFuncData* init_data) {
        if (
            common_init(
#if !DISABLE_ALL_LOGGING_FOR_BUILD
                init_data->log_type
#endif
            )
        ) {
            yes_tampering();
            return 0;
        }
        return 1;
    }
    
    // thcrap init function
    // Thcrap already removes the tamper protection,
    // so that code is unnecessary to include here.
    dll_export void cdecl netcode_mod_init(void* param) {
        common_init(
#if !DISABLE_ALL_LOGGING_FOR_BUILD
            enable_thcrap_console ? LOG_TO_PARENT_CONSOLE : NO_LOGGING
#endif
        );
    }
    
    // thcrap plugin init
    dll_export int stdcall thcrap_plugin_init() {
        if (HMODULE thcrap_handle = GetModuleHandleW(L"thcrap.dll")) {
            if (auto runconfig_game_get = (const char*(*)())GetProcAddress(thcrap_handle, "runconfig_game_get")) {
                const char* game_str = runconfig_game_get();
                if (game_str && !strcmp(game_str, "th155")) {
#if !DISABLE_ALL_LOGGING_FOR_BUILD
                    if (printf_t* log_func = (printf_t*)GetProcAddress(thcrap_handle, "log_printf")) {
                        log_printf = log_func;
                    }
#endif
                    if (mbox_t* log_func = (mbox_t*)GetProcAddress(thcrap_handle, "log_mbox")) {
                        log_mbox = log_func;
                    }
                    /*
                    if (runconfig_runcfg_fn_get_t* runcfg_get = (runconfig_runcfg_fn_get_t*)GetProcAddress(thcrap_handle, "runconfig_runcfg_fn_get")) {
                        if (const char* runcfg_fn = runcfg_get()) {
                            if (runconfig_thcrap_dir_get_t* thcrap_dir_get = (runconfig_thcrap_dir_get_t*)GetProcAddress(thcrap_handle, "runconfig_thcrap_dir_get")) {
                                if (const char* thcrap_dir = thcrap_dir_get()) {
                                    size_t length = snprintf(NULL, 0, "\"%sthcrap_loader.exe\" %s \"%s\"", thcrap_dir, game_str, runcfg_fn);
                                    sprintf(launch_str = (char*)malloc(length + 1), "\"%sthcrap_loader.exe\" %s \"%s\"", thcrap_dir, game_str, runcfg_fn);
                                }
                            }
                        }
                    }
                    */
                    //if (patchhook_register_t* patchhook_register_func = (patchhook_register_t*)GetProcAddress(thcrap_handle, "patchhook_register")) {
                        //patchhook_register = patchhook_register_func;
                    //}
#if !DISABLE_ALL_LOGGING_FOR_BUILD
                    if (globalconfig_get_boolean_t* globalconfig_get_boolean = (globalconfig_get_boolean_t*)GetProcAddress(thcrap_handle, "globalconfig_get_boolean")) {
                        enable_thcrap_console = globalconfig_get_boolean("console", false);
                    }
#endif
                    return 0;
                }
            }
        }
        return 1;
    }
}

void D3DX11CreateShaderResourceViewFromMemory() {
    // dummy export, will be overwritten with the real entry in the IAT
    unreachable;
}

static uint8_t orig_entrypoint[6];
static UINT __stdcall entrypoint_hook() {
    InitFuncData init_data = {
        .log_type = NO_LOGGING
    };
    netcode_init(&init_data);
    mem_write(entrypoint_base_addr, orig_entrypoint);
    __attribute__((musttail)) return ((UINT(__stdcall*)())entrypoint_base_addr)();
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD dwReason, LPVOID reserved) {
    /*
    if (dwReason == DLL_PROCESS_DETACH) {
        //Cleanup();
    }
    */
    if (dwReason == DLL_PROCESS_ATTACH) {
        wchar_t dll_path[1024];
        GetModuleFileNameW(hinst, dll_path, _countof(dll_path));

        wchar_t* dll_name;
        for (dll_name = &dll_path[wcslen(dll_path)]; dll_name != dll_path; dll_name--) {
            if (*dll_name == '\\' || *dll_name == '/') {
                dll_name++;
                break;
            }
        }
        if (_wcsicmp(dll_name, L"d3dx11_43.dll"))
            return TRUE;

        wchar_t d3dx_path[MAX_PATH] = {};
        GetWindowsDirectoryW(d3dx_path, MAX_PATH);
        wcscat_s(d3dx_path, L"\\System32\\d3dx11_43.dll");

        HMODULE d3dx11_43 = LoadLibraryW(d3dx_path);
        if (!d3dx11_43) {
            MessageBoxA(NULL, "Failed to load original d3dx11_43.dll", NULL, MB_ICONERROR);
            return FALSE;
        }
        hotpatch_import(D3DX11CreateShaderResourceViewFromMemory_import_addr, GetProcAddress(d3dx11_43, "D3DX11CreateShaderResourceViewFromMemory"));

        memcpy(orig_entrypoint, (void*)entrypoint_base_addr, sizeof(orig_entrypoint));
        hotpatch_jump(entrypoint_base_addr, entrypoint_hook);
    }
    return TRUE;
}
