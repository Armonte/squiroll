#include "TF4.h"
#include <cstdarg>
#include <cstdint>
#if __INTELLISENSE__
#undef _HAS_CXX20
#define _HAS_CXX20 0
#endif

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <squirrel.h>
#include <windows.h>
#include <filesystem>
namespace fs = std::filesystem;

#include <string>

#include "kite_api.h"
#include "patch_utils.h"

// #include "../shared/shared.h"
#include "util.h"
#include "log.h"
#include "netcode.h"
#include "config.h"
#include "file_replacement.h"
#include "alloc_man.h"
#include "lobby.h"
#include "sq_debug.h"
#include "gekko_bridge.h"
#include "discord.h"
#include "overlay.h"
#include "frame_data_display.h"
#include "rollback.h"

const KiteSquirrelAPI* KITE;

struct HostEnvironment;

typedef int thisfastcall get_value_t(
    const HostEnvironment* self,
    thisfastcall_edx(int dummy_edx,)
    const char* name,
    void** out
);

// size: 0x10
struct HostEnvironmentVftable {
    void *const __validate_environment; // 0x0
    void *const __method_4; // 0x4
    void *const __method_8; // 0x4
    get_value_t *const get_value; // 0xC
    // 0x10
};

// size: unknown
struct HostEnvironment {
    const HostEnvironmentVftable* vftable;

protected:
    template<typename T = void*>
    inline bool thiscall get_value(const char* name, T* out) const {
        return SUCCEEDED(this->vftable->get_value(
            this,
            thisfastcall_edx(0,)
            name,
            (void**)out
        ));
    }

public:

    inline bool get_hwnd(HWND& out) const {
        return this->get_value("HWND", &out);
    }

    inline bool get_hinstance(HINSTANCE& out) const {
        return this->get_value("HINSTANCE", &out);
    }

    // this just returns "Windows"
    inline bool get_platform(const char*& out) const {
        return this->get_value("PLATFORM", &out);
    }

    inline bool get_squirrel_vm(HSQUIRRELVM& out) const {
        return this->get_value("HSQUIRRELVM", &out);
    }

    inline bool get_kite_api(const KiteSquirrelAPI*& out) const {
        return this->get_value("Kite_SquirrelAPI", &out);
    }

    inline bool get_file_open_read_func(void*& out) const {
        return this->get_value("Function_FileOpenRead", &out);
    }

    inline bool get_file_open_write_func(void*& out) const {
        return this->get_value("Function_FileOpenWrite", &out);
    }

    inline bool get_interface_object_graphics(void*& out) const {
        return this->get_value("InterfaceObject_Graphics", &out);
    }

    inline bool get_interface_object_memory(void*& out) const {
        return this->get_value("InterfaceObject_Memory", &out);
    }
};

// SQInteger example_function(HSQUIRRELVM v) {
//     SQBool arg;
//     if (SQ_SUCCEEDED(sq_getbool(v, 2, &arg))) {
//         log_printf("function called from Squirrel with argument: %d\n", arg);
//         sq_pushbool(v, arg); // Return the argument
//     } else {
//         log_printf("function called from Squirrel with no arguments.\n");
//         return 0;
//     }
//     return 1; // Number of return values
// }

HSQUIRRELVM v;

bool sq_eval(HSQUIRRELVM v, const SQChar *code, bool get_result = false) {
    if (SQ_FAILED(sq_compilebuffer(v, code, -1, _SC("eval"), SQFalse)))return false;
    sq_pushroottable(v);
    if (SQ_FAILED(sq_call(v, 1, get_result, SQTrue)))return false;
    return true;
}

bool sq_eval(HSQUIRRELVM v, HSQOBJECT root, const SQChar *code, bool get_result = false) {
    if (SQ_FAILED(sq_compilebuffer(v, code, -1, _SC("eval"), SQFalse)))return false;
    sq_pushobject(v,root);
    if (SQ_FAILED(sq_call(v, 1, get_result, SQTrue)))return false;
    return true;
}

static inline void set_network_constants(HSQUIRRELVM v) {
    sq_setstring(v, _SC("config_section"),"network");
    sq_setbool(v, _SC("hide_opponent_name"), get_hide_name_enabled());
    sq_setbool(v,_SC("hide_ip"), get_hide_ip_enabled());
    sq_setbool(v, _SC("share_watch_ip"), get_share_watch_ip_enabled());
    sq_setbool(v, _SC("hide_profile_pictures"), get_hide_profile_pictures_enabled());
    // sq_setbool(v, _SC("auto_lobby_state_switch"), get_auto_switch());
    sq_setstring(v, _SC("blacklist"), get_network_blacklist());
    // Auto-connect (test-rig): if "host"/"client", boot.nut auto-pairs
    // two clients without going through the menu.
    sq_setstring(v, _SC("auto_connect"), get_auto_connect());
    sq_setstring(v, _SC("peer_ip"), get_peer_ip());
    sq_setinteger(v, _SC("peer_port"), get_peer_port());
    sq_setinteger(v, _SC("device_id"), get_device_id());
    sq_setbool(v, _SC("gekko_enabled"), get_gekko_enabled());
    //only add to config file if needed
    //sq_setbool(v, _SC("hide_lobby"), false);//more useful once we get custom lobbies
}

SQInteger update_network_constants(HSQUIRRELVM v) {
    sq_pushroottable(v);

    sq_edit(v, _SC("setting"),[](HSQUIRRELVM v){
        sq_edit(v, _SC("network"),set_network_constants);
    });

    sq_pop(v, 1);
    return 0;
}

SQInteger start_direct_punch_wait(HSQUIRRELVM v) {
    send_lobby_punch_wait();
    return 0;
}

SQInteger start_direct_punch_connect(HSQUIRRELVM v) {
    const SQChar* ip;
    SQInteger port;
    if (
        sq_gettop(v) == 3 &&
        SQ_SUCCEEDED(sq_getstring(v, 2, &ip)) &&
        SQ_SUCCEEDED(sq_getinteger(v, 3, &port))
    ) {
        send_lobby_punch_connect(false, ip, port);
    }
    return 0;
}

SQInteger inc_users_in_room(HSQUIRRELVM v) {
    ++users_in_room;
    return 0;
}

SQInteger dec_users_in_room(HSQUIRRELVM v) {
    if (users_in_room) {
        --users_in_room;
    }
    return 0;
}

SQInteger get_punch_ip(HSQUIRRELVM v) {
    punch_ip_updated = false;
    sq_pushstring(v, punch_ip_buffer, -1);
    return 1;
}

SQInteger reset_punch_ip(HSQUIRRELVM v) {
    *punch_ip_buffer = '\0';
    punch_ip_len = 0;
    punch_ip_updated = false;
    return 0;
}

static void set_clipboard(const char* str, size_t length) {
    OpenClipboard(NULL);
    EmptyClipboard();
    if (length) {
        ++length;
        HGLOBAL clip_mem = GlobalAlloc(GMEM_MOVEABLE, length);
        memcpy(GlobalLock(clip_mem), str, length);
        GlobalUnlock(clip_mem);
        SetClipboardData(CF_TEXT, clip_mem);
    }
    CloseClipboard();
}

SQInteger copy_punch_ip(HSQUIRRELVM v) {
    set_clipboard(punch_ip_buffer, punch_ip_len);
    return 0;
}

SQInteger copy_to_clipboard(HSQUIRRELVM v) {
    const SQChar* str;
    if (
        sq_gettop(v) == 2 &&
        SQ_SUCCEEDED(sq_getstring(v, 2, &str))
    ) {
        set_clipboard(str, strlen(str));
    }
    return 0;
}

SQInteger update_delay(HSQUIRRELVM v){
    SQInteger delay;
    if (
        sq_gettop(v) == 2 &&
        SQ_SUCCEEDED(sq_getinteger(v, 2, &delay))
    ) {
      latency = delay;
    }
    return 0;
}

SQInteger ignore_lobby_punch_ping(HSQUIRRELVM v) {
    respond_to_punch_ping = false;
    return 0;
}

// template<typename T>
// SQInteger ReceiveArray(HSQUIRRELVM v) {
//     if (sq_gettop(v) != 2 ||
//         sq_gettype(v, 2) != OT_ARRAY) {
//         return sq_throwerror(v, _SC("Expected one argument: array of MyClass instances"));
//     }

//     std::vector<T*> objects;

//     sq_push(v, 2);
//     sq_pushnull(v);
//     while (SQ_SUCCEEDED(sq_next(v, -2))) {
//         void* instance = nullptr;
//         sq_getinstanceup(v, -1, &instance, 0);
//         if (!instance) {
//             sq_pop(v, 4);
//             return sq_throwerror(v, _SC("Invalid MyClass instance in array"));
//         }
//         objects.emplace_back((T*)instance);
//         sq_pop(v, 2);
//     }
//     sq_pop(v, 1);
//     return 0;
// }


#define SQPUSH_BOOL_FUNC(val) [](HSQUIRRELVM v) -> SQInteger { sq_pushbool(v, (SQBool)(val)); return 1; }
#define SQPUSH_INT_FUNC(val) [](HSQUIRRELVM v) -> SQInteger { sq_pushinteger(v, (SQInteger)(val)); return 1; }
#define SQPUSH_FLOAT_FUNC(val) [](HSQUIRRELVM v) -> SQInteger { sq_pushfloat(v, (SQFloat)(val)); return 1; }

// gekko_bridge publishes the authoritative per-advance frame/rb/depth — the
// [nuttrace] native tags script-side trace values with these.
namespace gekko_bridge { extern int g_trace_frame; extern int g_trace_rb; extern int g_trace_depth; }

extern "C" {
    dll_export int stdcall init_instance_v2(HostEnvironment* environment) {
        if (
            environment &&
            environment->get_squirrel_vm(v) &&
            environment->get_kite_api(KITE)
        ) {
            // put any important initialization stuff here,
            // like adding squirrel globals/funcs/etc.
            sq_pushroottable(v);

#if !DISABLE_ALL_LOGGING_FOR_BUILD
            // Route Squirrel print() / error() through tee_printf so output
            // appears in BOTH the debug console and the on-disk log file.
            // Without this, ::print() in .nut files goes only to the console
            // and is lost on crash.
            sq_setprintfunc(v,
                (SQPRINTFUNCTION)sq_print_tee,
                (SQPRINTFUNCTION)sq_error_tee);
#endif

            sq_setcompilererrorhandler(v, [](HSQUIRRELVM v, const SQChar* desc, const SQChar* src, SQInteger line, SQInteger col) {
#if !DISABLE_ALL_LOGGING_FOR_BUILD
                log_printf(
                    "Squirrel compiler exception: %s\n"
                    "<%d,%d> \"%s\"\n",
                    src,
                    line, col, desc
                );
#else
                mboxf("Squirrel Exception", MB_OK | MB_ICONERROR, [=](auto add_text) {
                    add_text(
                        "Squirrel compiler exception: %s\n"
                        "<%d,%d> \"%s\"\n",
                        src,
                        line, col, desc
                    );
                });
#endif
            });
            sq_newclosure(v, [](HSQUIRRELVM v) -> SQInteger {
                if (sq_gettop(v) > 0) {
                    const SQChar* error_msg;
                    if (SQ_SUCCEEDED(sq_getstring(v, 2, &error_msg))) {
#if !DISABLE_ALL_LOGGING_FOR_BUILD
                        log_printf("Squirrel runtime exception: \"%s\"\n", error_msg);
                        SQStackInfos sqstack;
                        for (
                            SQInteger i = 1;
                            SQ_SUCCEEDED(sq_stackinfos(v, i, &sqstack));
                            ++i
                        ) {
                            log_printf(
                                *sqstack.source ? " %d | %s (%s)\n" : " %d | %s\n"
                                , sqstack.line, sqstack.funcname ? sqstack.funcname : "Anonymous function", sqstack.source
                            );
                        }
#else
                        mboxf("Squirrel Exception", MB_OK | MB_ICONERROR, [=](auto add_text) {
                            add_text("Squirrel runtime exception: \"%s\"\n", error_msg);
                            SQStackInfos sqstack;
                            for (
                                SQInteger i = 1;
                                SQ_SUCCEEDED(sq_stackinfos(v, i, &sqstack));
                                ++i
                            ) {
                                add_text(
                                    *sqstack.source ? " %d | %s (%s)\n" : " %d | %s\n"
                                    , sqstack.line, sqstack.funcname ? sqstack.funcname : "Anonymous function", sqstack.source
                                );
                            }
                        });
#endif
                    }
                }
                return 0;
            }, 0);
            sq_seterrorhandler(v);

            sq_setfunc(v, _SC("print"), sq_print);
            sq_setfunc(v, _SC("fprint"), sq_fprint);

            // [nuttrace] -- .nut-side divergence tracer. ::__gekko_trace(tag, val)
            // logs the value (float bits, or int) tagged with the authoritative
            // C++ gekko frame/rb/depth, so script state can be diffed fwd-vs-
            // resim in ONE run. Gated by SQUIROLL_NUT_TRACE (off => the native
            // returns immediately, so the .nut call sites cost ~nothing).
            sq_setfunc(v, _SC("__gekko_trace"), [](HSQUIRRELVM v) -> SQInteger {
                static int on = -1;
                if (on < 0) {
                    char b[8] = {0};
                    on = (GetEnvironmentVariableA("SQUIROLL_NUT_TRACE", b, sizeof b) > 0
                          && b[0] != '0') ? 1 : 0;
                }
                if (!on) return 0;
                // Frame-window gate (the f=24 divergence) to bound the volume.
                int f = gekko_bridge::g_trace_frame;
                if (f < 18 || f > 32) return 0;
                const SQChar* tag = nullptr;
                sq_getstring(v, 2, &tag);
                // String value -> log as text (used for getstackinfos caller names);
                // otherwise float bits / int.
                if (sq_gettype(v, 3) == OT_STRING) {
                    const SQChar* sv = nullptr;
                    sq_getstring(v, 3, &sv);
                    log_printf("[nuttrace] f=%d rb=%d d=%d %s='%s'\n",
                               gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb,
                               gekko_bridge::g_trace_depth, tag ? tag : "?", sv ? sv : "?");
                    return 0;
                }
                if (sq_gettype(v, 3) == OT_INTEGER) {
                    SQInteger iv = 0; sq_getinteger(v, 3, &iv);
                    log_printf("[nuttrace] f=%d rb=%d d=%d %s=%d\n",
                               gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb,
                               gekko_bridge::g_trace_depth, tag ? tag : "?", (int)iv);
                    return 0;
                }
                uint32_t vb = 0; SQFloat fv = 0; SQInteger iv = 0;
                if (SQ_SUCCEEDED(sq_getfloat(v, 3, &fv)))        __builtin_memcpy(&vb, &fv, 4);
                else if (SQ_SUCCEEDED(sq_getinteger(v, 3, &iv))) vb = (uint32_t)iv;
                log_printf("[nuttrace] f=%d rb=%d d=%d %s=%08X\n",
                           gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb,
                           gekko_bridge::g_trace_depth, tag ? tag : "?", vb);
                return 0;
            });

            sq_setfunc(v, _SC("system"),[](HSQUIRRELVM v) -> SQInteger {return 0;});
            sq_setfunc(v, _SC("mkdir"),[](HSQUIRRELVM v) -> SQInteger {
                const SQChar* path;
                if (sq_gettop(v) != 2 ||
                    SQ_FAILED(sq_getstring(v, 2, &path))
                ) {
                    return sq_throwerror(v, _SC("Invalid arguments, expected <path>"));
                }
                if(!CreateDirectory(path, NULL)) {
                    if(GetLastError() != ERROR_ALREADY_EXISTS) {
                        return sq_throwerror(v, _SC("Failed to create directory"));
                    }
                }
                sq_pushbool(v, SQTrue);
                return 1;
            });
            sq_setfunc(v, _SC("readfile"), [](HSQUIRRELVM v) -> SQInteger {
                const SQChar* path;
                if (sq_gettop(v) != 2 ||
                    SQ_FAILED(sq_getstring(v, 2, &path))
                ) {
                    return sq_throwerror(v, _SC("Invalid arguments, expected <path>"));
                }
                HANDLE hFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile == INVALID_HANDLE_VALUE) {
                    return sq_throwerror(v, _SC("Could not read file"));
                }

                DWORD fileSize = GetFileSize(hFile, NULL);
                if (fileSize == INVALID_FILE_SIZE) {
                    CloseHandle(hFile);
                    return sq_throwerror(v, _SC("Could not get file size"));
                }

                char *buffer = (char *)malloc(fileSize + 1);
                if (!buffer) {
                    CloseHandle(hFile);
                    return sq_throwerror(v, _SC("Memory allocation failed"));
                }

                DWORD bytesRead;
                if (!ReadFile(hFile, buffer, fileSize, &bytesRead, NULL) || bytesRead != fileSize) {
                    free(buffer);
                    CloseHandle(hFile);
                    return sq_throwerror(v, _SC("Could not read file"));
                }
                buffer[fileSize] = '\0';
                CloseHandle(hFile);

                sq_pushstring(v, buffer, fileSize);
                free(buffer);
                return 1;
            });
            sq_setfunc(v, _SC("writefile"), [](HSQUIRRELVM v) -> SQInteger {
                const SQChar* path;
                const SQChar* data;
                if (sq_gettop(v) != 3 ||
                    SQ_FAILED(sq_getstring(v, 2, &path)) ||
                    SQ_FAILED(sq_getstring(v, 3, &data))
                ) {
                    return sq_throwerror(v, _SC("Invalid arguments, expected <path> <data>"));
                }
                HANDLE hFile = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile == INVALID_HANDLE_VALUE) {
                    return sq_throwerror(v, _SC("Could not open file for writing"));
                }

                DWORD bytesWritten;
                DWORD dataLen = (DWORD)scstrlen(data);
                if (!WriteFile(hFile, data, dataLen, &bytesWritten, NULL) || bytesWritten != dataLen) {
                    CloseHandle(hFile);
                    return sq_throwerror(v, _SC("Could not write file"));
                }

                CloseHandle(hFile);
                sq_pushbool(v, SQTrue);
                return 1;
            });
            sq_setfunc(v, _SC("listfiles"), [](HSQUIRRELVM v) -> SQInteger {
                const SQChar* path;
                if (sq_gettop(v) != 2 ||
                    SQ_FAILED(sq_getstring(v, 2, &path))
                ) {
                    return sq_throwerror(v, _SC("Invalid arguments, expected <path>"));
                }
                try {
                    sq_newarray(v, 0);
                    for (const auto &entry : fs::directory_iterator(path)) {
                        if (fs::is_regular_file(entry.path())) {
                            std::string file = entry.path().filename().string();
                            sq_pushstring(v, file.c_str(), -1);
                            sq_arrayappend(v, -2);
                            // if (file.size() >= 4 && file.substr(file.size() - 4) == ".nut") {
                            // }
                        }
                    }
                    return 1;
                } catch (const std::exception &e) {
                    return sq_throwerror(v, e.what());
                }
            });
            sq_setfunc(v, _SC("deepcopy"), sq_deepcopy);

            // setting table setup
            sq_createtable(v, _SC("setting"), [](HSQUIRRELVM v) {
                sq_setinteger(v, _SC("version"), PLUGIN_VERSION);
                sq_setinteger(v, _SC("revision"), PLUGIN_REVISION);
                sq_setfunc(v,_SC("save"),[](HSQUIRRELVM v) -> SQInteger {
                    const char* section;
                    const char* key;
                    const char* value;
                    if (sq_gettop(v) != 4 ||
                        SQ_FAILED(sq_getstring(v, 2, &section)) ||
                        SQ_FAILED(sq_getstring(v, 3, &key)) ||
                        SQ_FAILED(sq_getstring(v, 4, &value))
                    ) {
                        return sq_throwerror(v, "Invalid arguments, expected: <section> <key> <value>");
                    }
                    set_config_string(section,key,value);
                    return 0;
                });
                sq_createtable(v, _SC("misc"), [](HSQUIRRELVM v){
                    sq_setbool(v, _SC("hide_wip"), get_hide_wip_enabled());
                    sq_setbool(v, _SC("skip_intro"), get_skip_intro_enabled()); // This isn't a function because it only runs once anyway
                    sq_setbool(v, _SC("skip_to_battle"), get_skip_to_battle_enabled());
                });
                sq_createtable(v, _SC("network"),[](HSQUIRRELVM v){
                    sq_setfunc(v, _SC("update_consts"), update_network_constants);
                    set_network_constants(v);
                    // gekko_init(local_port, remote_port, local_idx, remote_ip)
                    // Starts the GekkoNet rollback session. Call from
                    // boot.nut once auto_connect's CSS override has fired
                    // vs.Initialize on both sides.
                    sq_setfunc(v, _SC("gekko_init"), [](HSQUIRRELVM v) -> SQInteger {
                        SQInteger local_port, remote_port, local_idx;
                        const SQChar* remote_ip;
                        if (sq_gettop(v) != 5 ||
                            SQ_FAILED(sq_getinteger(v, 2, &local_port)) ||
                            SQ_FAILED(sq_getinteger(v, 3, &remote_port)) ||
                            SQ_FAILED(sq_getinteger(v, 4, &local_idx)) ||
                            SQ_FAILED(sq_getstring(v, 5, &remote_ip))
                        ) {
                            return sq_throwerror(v,
                                "Invalid arguments, expected: "
                                "<local_port:int> <remote_port:int> "
                                "<local_idx:int> <remote_ip:string>");
                        }
                        bool ok = gekko_bridge::init(
                            (uint16_t)local_port, (uint16_t)remote_port,
                            (uint8_t)local_idx, remote_ip);
                        sq_pushbool(v, ok ? SQTrue : SQFalse);
                        return 1;
                    });
                    // gekko_watch_for_fight_solo() — single-process stress
                    // session. boot.nut calls this AFTER vs.Initialize so
                    // the intro runs vanilla; the GekkoStressSession is
                    // created when battle.state hits Round_Fight.
                    sq_setfunc(v, _SC("gekko_watch_for_fight_solo"), [](HSQUIRRELVM v) -> SQInteger {
                        gekko_bridge::watch_for_fight_solo();
                        return 0;
                    });
                    // gekko_watch_for_fight_dual(local_port, remote_port,
                    // local_idx, remote_ip) — dual netplay. boot.nut calls
                    // this AFTER vs.Initialize; the GekkoGameSession is
                    // created when battle.state hits Round_Fight.
                    sq_setfunc(v, _SC("gekko_watch_for_fight_dual"), [](HSQUIRRELVM v) -> SQInteger {
                        SQInteger local_port, remote_port, local_idx;
                        const SQChar* remote_ip;
                        if (sq_gettop(v) != 5 ||
                            SQ_FAILED(sq_getinteger(v, 2, &local_port)) ||
                            SQ_FAILED(sq_getinteger(v, 3, &remote_port)) ||
                            SQ_FAILED(sq_getinteger(v, 4, &local_idx)) ||
                            SQ_FAILED(sq_getstring(v, 5, &remote_ip))
                        ) {
                            return sq_throwerror(v,
                                "Invalid arguments, expected: "
                                "<local_port:int> <remote_port:int> "
                                "<local_idx:int> <remote_ip:string>");
                        }
                        gekko_bridge::watch_for_fight_dual(
                            (uint16_t)local_port, (uint16_t)remote_port,
                            (uint8_t)local_idx, remote_ip);
                        return 0;
                    });
                    sq_setfunc(v, _SC("gekko_shutdown"), [](HSQUIRRELVM v) -> SQInteger {
                        gekko_bridge::shutdown();
                        return 0;
                    });
                    sq_setfunc(v, _SC("gekko_is_active"), [](HSQUIRRELVM v) -> SQInteger {
                        sq_pushbool(v, gekko_bridge::is_active() ? SQTrue : SQFalse);
                        return 1;
                    });
                    // GekkoSessionStarted fires when both peers have
                    // completed the sync handshake. Boot.nut polls this
                    // before calling vs.Initialize so the engine's first
                    // battle frame happens under gekko ownership on both
                    // peers at the same wall-clock moment.
                    sq_setfunc(v, _SC("gekko_session_started"), [](HSQUIRRELVM v) -> SQInteger {
                        sq_pushbool(v, gekko_bridge::is_session_started() ? SQTrue : SQFalse);
                        return 1;
                    });
                });
                sq_createtable(v, _SC("frame_data"), [](HSQUIRRELVM v) {
                    sq_setfunc(v, _SC("IsFrameActive"), [](HSQUIRRELVM v) -> SQInteger {
                        void* inst;
                        if (sq_gettop(v) != 2 ||
                            SQ_FAILED(sq_getinstanceup(v, 2, &inst, nullptr))
                        ) {
                            return sq_throwerror(v, "Invalid arguments, expected: <player>");
                        }
                        sq_pushbool(v, IsFrameActive((ManbowActor2D*)inst));
                        return 1;
                    });
                    sq_setfunc(v, _SC("GetMetadata"), [](HSQUIRRELVM v)-> SQInteger {
                        void* player;
                        if (sq_gettop(v) != 2 ||
                            SQ_FAILED((sq_getinstanceup(v, 2, &player, nullptr)))
                        ) {
                            return sq_throwerror(v, "Invalid arguments, expected: <player>");
                        }
                        int16_t* metadata = GetMetadata((ManbowActor2D*)player);
                        sq_newarray(v, 0);
                        for(int i = 0; i <= 23; ++i){
                            sq_pushinteger(v, metadata[i]);
                            sq_arrayappend(v, -2);
                        }
                        return 1;
                    });
                    sq_setfunc(v, _SC("hasData"), [](HSQUIRRELVM v) -> SQInteger {
                        void* inst;
                        if (sq_gettop(v) != 2 ||
                            SQ_FAILED(sq_getinstanceup(v, 2, &inst, nullptr))
                        ) {
                            return sq_throwerror(v, "Invalid arguments, expected: <instance>");
                        }
                        sq_pushbool(v, hasData((ManbowActor2D*)inst));
                        return 1;
                    });
                    sq_setfunc(v, _SC("NewMove"), [](HSQUIRRELVM v) -> SQInteger {
                        void* inst;
                        if (sq_gettop(v) != 2 ||
                            SQ_FAILED(sq_getinstanceup(v, 2, &inst, nullptr))
                        ) {
                            return sq_throwerror(v, "Invalid arguments, expected: <instance>");
                        }
                        sq_pushbool(v, NewTake((ManbowActor2D*)inst));
                        return 1;
                    });
                });
            });

            sq_createtable(v, _SC("math"),[](HSQUIRRELVM v) {
                sq_setfunc(v,_SC("fclamp"),[](HSQUIRRELVM v) -> SQInteger {
                    SQFloat Val,minVal,maxVal;
                    if (sq_gettop(v) != 4 ||
                        SQ_FAILED(sq_getfloat(v, 2, &Val)) ||
                        SQ_FAILED(sq_getfloat(v, 3, &minVal)) ||
                        SQ_FAILED(sq_getfloat(v, 4, &maxVal))
                    ) {
                        return sq_throwerror(v, "Invalid arguments, expected: <Val> <minVal> <maxVal>");
                    }
                    SQFloat result = Val < minVal ? minVal : (Val > maxVal ? maxVal : Val);
                    sq_pushfloat(v, result);
                    return 1;
                });
                sq_setfunc(v, _SC("fmin"),[](HSQUIRRELVM v) -> SQInteger {
                    SQFloat a,b;
                    if (sq_gettop(v) != 3 ||
                        SQ_FAILED(sq_getfloat(v, 2, &a)) ||
                        SQ_FAILED(sq_getfloat(v, 3, &b))
                    ) {
                        return sq_throwerror(v, "Invalid arguments, expected: <a> <b>");
                    }
                    sq_getfloat(v, 2, &a);
                    sq_getfloat(v, 3, &b);

                    sq_pushfloat(v, a < b ? a : b);
                    return 1;
                });
                sq_setfunc(v, _SC("fmax"),[](HSQUIRRELVM v) -> SQInteger {
                    SQFloat a,b;
                    if (sq_gettop(v) != 3 ||
                        SQ_FAILED(sq_getfloat(v, 2, &a)) ||
                        SQ_FAILED(sq_getfloat(v, 3, &b))
                    ) {
                        return sq_throwerror(v, "Invalid arguments, expected: <a> <b>");
                    }
                    sq_getfloat(v, 2, &a);
                    sq_getfloat(v, 3, &b);

                    sq_pushfloat(v, a > b ? a : b);
                    return 1;
                });
                sq_setfunc(v,_SC("clamp"),[](HSQUIRRELVM v) -> SQInteger {
                    SQInteger Val,minVal,maxVal;
                    if (sq_gettop(v) != 4 ||
                        SQ_FAILED(sq_getinteger(v, 2, &Val)) ||
                        SQ_FAILED(sq_getinteger(v, 3, &minVal)) ||
                        SQ_FAILED(sq_getinteger(v, 4, &maxVal))
                    ) {
                        return sq_throwerror(v, "Invalid arguments, expected: <Val> <minVal> <maxVal>");
                    }
                    SQInteger result = Val < minVal ? minVal : (Val > maxVal ? maxVal : Val);
                    sq_pushinteger(v, result);
                    return 1;
                });
                sq_setfunc(v, _SC("min"),[](HSQUIRRELVM v) -> SQInteger {
                    SQInteger a,b;
                    if (sq_gettop(v) != 3 ||
                        SQ_FAILED(sq_getinteger(v, 2, &a)) ||
                        SQ_FAILED(sq_getinteger(v, 3, &b))
                    ) {
                        return sq_throwerror(v, "Invalid arguments, expected: <a> <b>");
                    }
                    sq_getinteger(v, 2, &a);
                    sq_getinteger(v, 3, &b);

                    sq_pushinteger(v, a < b ? a : b);
                    return 1;
                });
                sq_setfunc(v, _SC("max"),[](HSQUIRRELVM v) -> SQInteger {
                    SQInteger a,b;
                    if (sq_gettop(v) != 3 ||
                        SQ_FAILED(sq_getinteger(v, 2, &a)) ||
                        SQ_FAILED(sq_getinteger(v, 3, &b))
                    ) {
                        return sq_throwerror(v, "Invalid arguments, expected: <a> <b>");
                    }
                    sq_getinteger(v, 2, &a);
                    sq_getinteger(v, 3, &b);

                    sq_pushinteger(v, a > b ? a : b);
                    return 1;
                });
                sq_setfunc(v, _SC("rgbaToHex"), [](HSQUIRRELVM v) -> SQInteger {
                    SQFloat r, g, b, a;
                    if (sq_gettop(v) != 5 ||
                        SQ_FAILED(sq_getfloat(v, 2, &r)) ||
                        SQ_FAILED(sq_getfloat(v, 3, &g)) ||
                        SQ_FAILED(sq_getfloat(v, 4, &b)) ||
                        SQ_FAILED(sq_getfloat(v, 5, &a))
                    ) {
                        return sq_throwerror(v, _SC("Expected: <r> <g> <b> <a>"));
                    }
                    char hex[10];
                    snprintf(hex, sizeof(hex),"%02X%02X%02X%02X",
                        (int)(a * 255),(int)(r * 255), (int)(g * 255), (int)(b * 255));
                    sq_pushstring(v, hex, -1);
                    return 1;
                });
            });

            // rollback table setup
            sq_createtable(v, _SC("rollback"), [](HSQUIRRELVM v) {
				sq_setfunc(v, _SC("update_delay"), update_delay);
                sq_setfunc(v, _SC("resyncing"), SQPUSH_BOOL_FUNC(resyncing));
                sq_setfunc(v, _SC("get_buffered_frames"), SQPUSH_INT_FUNC(local_buffered_frames));
				sq_setfunc(v, _SC("start"), [](HSQUIRRELVM v) -> SQInteger {
                    rollback_start();
                    return 1;
                });
                sq_setfunc(v, _SC("stop"), [](HSQUIRRELVM v) -> SQInteger {
                    rollback_stop();
                    return 1;
                });
                sq_setfunc(v, _SC("preframe"), [](HSQUIRRELVM v) -> SQInteger {
                    rollback_preframe();
                    return 1;
                });
                sq_setfunc(v, _SC("postframe"), [](HSQUIRRELVM v) -> SQInteger {
                    rollback_postframe();
                    return 1;
                });
                sq_setfunc(v, _SC("rewind"), [](HSQUIRRELVM v) -> SQInteger {
                    SQInteger frames;
                    if (sq_gettop(v) != 2 ||
                        SQ_FAILED(sq_getinteger(v, 2, &frames))
                    ) {
                        return sq_throwerror(v, _SC("Invalid arguments, expected: <frames>"));
                    }
                    rollback_rewind(frames);
                    return 0;
                });
				sq_setfunc(v, _SC("rewindA"), [](HSQUIRRELVM v) -> SQInteger {
					SQInteger frames;
					if (sq_gettop(v) != 2 ||
						SQ_FAILED(sq_getinteger(v, 2, &frames))
					) {
						return sq_throwerror(v, _SC("SYNTAX_ERROR@::rollback.rewindA: <frames>"));
					}
					auto res = rollback_allocs(frames);
					sq_pushinteger(v,res);
					return 1;
				});
				sq_setfunc(v, _SC("getactors"), [](HSQUIRRELVM v) -> SQInteger {
						
					return 1;
				});
            });

            sq_createtable(v, _SC("punch"), [](HSQUIRRELVM v) {
                sq_setfunc(v, _SC("init_wait"), start_direct_punch_wait);
                sq_setfunc(v, _SC("init_connect"), start_direct_punch_connect);
                sq_setfunc(v, _SC("get_ip"), get_punch_ip);
                sq_setfunc(v, _SC("reset_ip"), reset_punch_ip);
                sq_setfunc(v, _SC("ip_available"), SQPUSH_BOOL_FUNC(punch_ip_updated));
                sq_setfunc(v, _SC("copy_ip_to_clipboard"), copy_punch_ip);
                sq_setfunc(v, _SC("ignore_ping"), ignore_lobby_punch_ping);
            });

            // debug table setup
            sq_createtable(v, _SC("debug"), [](HSQUIRRELVM v) {
                sq_setfunc(v, _SC("print_value"), sq_print_value);
                sq_setfunc(v, _SC("fprint_value"), sq_fprint_value);
                sq_setfunc(v, _SC("dev"), SQPUSH_BOOL_FUNC(get_dev_mode_enabled()));
                sq_setfunc(v, _SC("hash"), [](HSQUIRRELVM v) -> SQInteger {
                    if (sq_gettop(v) != 2)
                        return sq_throwerror(v, _SC("Invalid arguments, expected: <object>"));

                    SQHash h = sq_gethash(v, 2);
                    sq_pushinteger(v, (SQInteger)h);
                    return 1;
                });
                sq_setfunc(v, _SC("test"), [](HSQUIRRELVM v) -> SQInteger {
                    void* player;
                    if (sq_gettop(v) != 2 ||
                        SQ_FAILED(sq_getinstanceup(v, 2, &player, nullptr))
                    ) {
                        return sq_throwerror(v, "Invalid arguments, expected: <player>");
                    }
                    debug((ManbowActor2D*)player);
                    return 0;
                });
            });

            // modifications to the manbow table
            sq_edit(v, _SC("manbow"), [](HSQUIRRELVM v) {
                HSQOBJECT CompileFile;

                sq_pushstring(v, _SC("CompileFile"), -1);
                sq_get(v, -2);
                sq_getstackobj(v, -1, &CompileFile);
                sq_pop(v, 1);

                sq_pushstring(v, _SC("orig_compilefile"), -1);
                sq_pushobject(v, CompileFile);
                sq_rawset(v, -3);

                sq_pushstring(v, _SC("CompileFile"), -1);
                sq_newclosure(v, [](HSQUIRRELVM v) -> SQInteger {
                    const SQChar* file;
                    HSQOBJECT root;

                    if (SQ_FAILED(sq_getstring(v, 2, &file))) return sq_throwerror(v, _SC("Invalid file path"));
                    if (SQ_FAILED(sq_getstackobj(v, 3, &root))) return sq_throwerror(v, _SC("Invalid root object"));
                    if (GetEnvironmentVariableA("SQUIROLL_NUT_TRACE", nullptr, 0))
                        log_printf("[cf] %s\n", file);   // TEMP: trace every CompileFile

                    if (EmbedData embed = get_embed_data(file)) {
                        // log_printf("found %s in buffer,compiling...\n",file);
                        if (SQ_SUCCEEDED(sq_compilebuffer(v, (const SQChar *)embed.data,embed.length, file, SQTrue))) {
                            sq_pushobject(v, root);
                            sq_call(v, 1, SQFalse, SQTrue);
                            goto apply_patches;
                        }
                    }

                    HSQOBJECT manbow;
                    sq_pushroottable(v);
                    sq_pushstring(v, _SC("manbow"), -1);
                    sq_get(v, -2);
                    sq_getstackobj(v, -1, &manbow);
                    sq_pushstring(v, _SC("orig_compilefile"), -1);
                    if (SQ_FAILED(sq_get(v, -2))) return sq_throwerror(v, _SC("orig_func not found"));

                    sq_pushobject(v, manbow);
                    sq_pushstring(v, file, -1);
                    sq_pushobject(v, root);
                    if (SQ_FAILED(sq_call(v, 3, SQFalse, SQTrue))) return sq_throwerror(v, _SC("call failed"));

                    apply_patches:
                        sq_pushroottable(v);
                        sq_pushstring(v, _SC("plugin"), -1);
                        sq_get(v, -2);
                        sq_pushstring(v, _SC("patches"), -1);
                        sq_get(v, -2);
                        sq_pushstring(v, file, -1);
                        if (SQ_SUCCEEDED(sq_get(v, -2))) {
                            log_printf("found patch for %s,applying...\n",file);
                            sq_pushobject(v, root);
                            if (SQ_FAILED(sq_call(v, 1, SQFalse, SQTrue)))return sq_throwerror(v, _SC("failed to apply patch"));
                        }

                    // [nuttrace] wrap Actor2D.VX_Brake the instant actor_game.nut
                    // finishes compiling into ::manbow.Actor2D (root), BEFORE any
                    // DerivedClass(Actor2D) copies the method table. Done from C++
                    // (not ::plugin.patches) because the actor base classes compile
                    // BEFORE the .nut plugin patch registration runs, so a .nut
                    // Patch() would miss this file. The wrapper calls the original
                    // (_vxb) so behaviour is unchanged; ::__gekko_trace gates on
                    // SQUIROLL_NUT_TRACE + a frame window. Self-balancing on the SQ
                    // stack (compilebuffer +1, push root +1, call -1, pop -1).
                    if (strcmp(file, "data/actor/script/actor_game.nut") == 0) {
                        static const char WRAP[] =
                            "if (!(\"__vxbhit_n\" in ::getroottable())) ::__vxbhit_n <- 0;\n"
                            "local _vxb = VX_Brake;\n"
                            "function VX_Brake(x_, min_ = null) {\n"
                            "  if (::__vxbhit_n < 40) { ::__vxbhit_n = ::__vxbhit_n + 1; ::print(\"[vxbhit] VX_Brake invoked\\n\"); }\n"
                            "  ::__gekko_trace(\"VXB_in_vax\", this.va.x);\n"
                            "  ::__gekko_trace(\"VXB_x\", x_);\n"
                            "  ::__gekko_trace(\"VXB_min\", min_ == null ? -99999.0 : min_);\n"
                            // Name the .nut call chain ABOVE VX_Brake (the move/state handler
                            // that set the divergent pre-brake va.x). getstackinfos(N): N=1 is
                            // this wrapper, N>=2 the callers. Log func+line (+src for L2).
                            "  local s2 = ::getstackinfos(2); if (s2) { ::__gekko_trace(\"VXB_c2\", s2.func); ::__gekko_trace(\"VXB_c2_ln\", s2.line); ::__gekko_trace(\"VXB_c2_src\", s2.src); }\n"
                            "  local s3 = ::getstackinfos(3); if (s3) { ::__gekko_trace(\"VXB_c3\", s3.func); ::__gekko_trace(\"VXB_c3_ln\", s3.line); }\n"
                            "  local s4 = ::getstackinfos(4); if (s4) { ::__gekko_trace(\"VXB_c4\", s4.func); ::__gekko_trace(\"VXB_c4_ln\", s4.line); }\n"
                            "  local r = _vxb.call(this, x_, min_);\n"
                            "  ::__gekko_trace(\"VXB_vx\", this.vx);\n"
                            "  return r;\n"
                            "}\n"
                            // Wrap the va.x setters too -- log va.x AFTER each, so the one whose
                            // post-value is the divergent brake input (10.0 fwd / 5.0 d=1) is the
                            // writer. If none diverge, va.x is a direct `this.va.x = ...` write.
                            "local _ssxy = SetSpeed_XY;\n"
                            "function SetSpeed_XY(a, b) { local r = _ssxy.call(this, a, b); ::__gekko_trace(\"SET_SSXY_vax\", this.va.x); return r; }\n"
                            "local _ssv = SetSpeed_Vec;\n"
                            "function SetSpeed_Vec(a, b, c = 1.0) { local r = _ssv.call(this, a, b, c); ::__gekko_trace(\"SET_SSV_vax\", this.va.x); return r; }\n"
                            "local _asxy = AddSpeed_XY;\n"
                            "function AddSpeed_XY(a, b, c = null, d = null) { local r = _asxy.call(this, a, b, c, d); ::__gekko_trace(\"SET_ASXY_vax\", this.va.x); return r; }\n"
                            "local _asv = AddSpeed_Vec;\n"
                            "function AddSpeed_Vec(a, b, c, d = 1.0) { local r = _asv.call(this, a, b, c, d); ::__gekko_trace(\"SET_ASV_vax\", this.va.x); return r; }\n";
                        if (SQ_SUCCEEDED(sq_compilebuffer(v, WRAP, (SQInteger)(sizeof(WRAP) - 1), _SC("vxb_wrap"), SQTrue))) {
                            sq_pushobject(v, root);
                            if (SQ_FAILED(sq_call(v, 1, SQFalse, SQTrue)))
                                log_printf("[nuttrace] VX_Brake wrap FAILED to apply\n");
                            else
                                log_printf("[nuttrace] VX_Brake wrapped (actor_game.nut)\n");
                            sq_pop(v, 1);   // pop the compiled closure
                        } else {
                            log_printf("[nuttrace] VX_Brake wrap COMPILE error\n");
                        }
                    }

                    // [nuttrace] wrap player MainLoop (player_update.nut -> the player
                    // class) to log va.x at f=24 ENTRY -- settles whether the divergent
                    // pre-brake va.x (10.0 fwd / 5.0 d=1) is CARRIED from f=23 (identical
                    // at entry => set during f=24) or already divergent at entry (=> an
                    // un-captured carry-over the snapshot misses).
                    if (strcmp(file, "data/actor/script/player_update.nut") == 0) {
                        static const char WRAP2[] =
                            "local _mlf = MainLoopFirst;\n"
                            "function MainLoopFirst() { ::__gekko_trace(\"MLF_entry_vax\", this.va.x); local r = _mlf.call(this); ::__gekko_trace(\"MLF_exit_vax\", this.va.x); return r; }\n"
                            "local _ml = MainLoop;\n"
                            "function MainLoop() { ::__gekko_trace(\"ML_entry_vax\", this.va.x); return _ml.call(this); }\n"
                            "local _mlc = MainLoopCount;\n"
                            "function MainLoopCount() { ::__gekko_trace(\"MLC_entry_vax\", this.va.x); return _mlc.call(this); }\n";
                        if (SQ_SUCCEEDED(sq_compilebuffer(v, WRAP2, (SQInteger)(sizeof(WRAP2) - 1), _SC("ml_wrap"), SQTrue))) {
                            sq_pushobject(v, root);
                            if (SQ_FAILED(sq_call(v, 1, SQFalse, SQTrue)))
                                log_printf("[nuttrace] MainLoop wrap FAILED\n");
                            else
                                log_printf("[nuttrace] MainLoop wrapped (player_update.nut)\n");
                            sq_pop(v, 1);
                        }
                    }
                    return 0;

                }, 0);
                sq_rawset(v, -3);

                sq_setfunc(v, _SC("SetClipboardString"), copy_to_clipboard);
            });

            // custom lobby table
            sq_createtable(v, _SC("lobby"), [](HSQUIRRELVM v) {
                sq_setfunc(v, _SC("user_count"), SQPUSH_INT_FUNC(users_in_room));
                sq_setfunc(v, _SC("inc_user_count"), inc_users_in_room);
                sq_setfunc(v, _SC("dec_user_count"), dec_users_in_room);
            });

            // discord rich presence

#if ENABLE_DISCORD_INTEGRATION
#define DISCORD_RPC_FUNC(field) \
[](HSQUIRRELVM v) -> SQInteger { \
    const SQChar* str; \
    if (sq_gettop(v) != 2 || SQ_FAILED(sq_getstring(v, 2, &str))) { \
        return sq_throwerror(v, "Invalid arguments, expected: <string>"); \
    } \
    MACRO_CAT(discord_rpc_set_,field)(str); \
    return 0; \
}
#else
#define DISCORD_RPC_FUNC(field) sq_dummy
#endif

            #define RPC_FIELD(field) \
                sq_setfunc(v, _SC("rpc_set_" MACRO_STR(field)), DISCORD_RPC_FUNC(field));

            sq_createtable(v, _SC("discord"), [](HSQUIRRELVM v) {
                RPC_FIELDS
                sq_setfunc(v, _SC("rpc_commit_details_and_state"),
#if ENABLE_DISCORD_INTEGRATION
                    [](HSQUIRRELVM v) -> SQInteger {
                        const SQChar* details;
                        const SQChar* state;
                        if (sq_gettop(v) != 3 || SQ_FAILED(sq_getstring(v, 2, &details)) || SQ_FAILED(sq_getstring(v, 3, &state))) {
                            return sq_throwerror(v, "Invalid arguments, expected: <string> <string>");
                        }
                        discord_rpc_set_details(details);
                        discord_rpc_set_state(state);
                        discord_rpc_commit();
                        return 0;
                    }
#else
                    sq_dummy
#endif
                );
                sq_setfunc(v, _SC("rpc_commit"),
#if ENABLE_DISCORD_INTEGRATION
                    [](HSQUIRRELVM v) -> SQInteger {
                        discord_rpc_commit();
                        return 0;
                    }
#else
                    sq_dummy
#endif
                );
                sq_setbool(v, _SC("enabled"), get_discord_enabled());
            });
            #undef RPC_FIELD

            // custom render overlays
            sq_createtable(v, _SC("overlay"), [](HSQUIRRELVM v) {
                sq_setbool(v, _SC("enabled"), get_hitbox_vis_enabled());
                sq_setfunc(v, _SC("set_hitboxes"), [](HSQUIRRELVM v) -> SQInteger {
                    void* inst;
                    SQInteger p1_flags;
                    SQInteger p2_flags;
                    if (sq_gettop(v) != 4 ||
                        SQ_FAILED(sq_getinstanceup(v, 2, &inst, nullptr)) ||
                        SQ_FAILED(sq_getinteger(v, 3, &p1_flags)) ||
                        SQ_FAILED(sq_getinteger(v, 4, &p2_flags))
                    ) {
                        return sq_throwerror(v, "Invalid arguments, expected: <instance> <integer> <integer>");
                    }
                    overlay_set_hitboxes((ManbowActor2DGroup*)inst, p1_flags, p2_flags);
                    return 0;
                });
                sq_setfunc(v, _SC("clear"), [](HSQUIRRELVM v) -> SQInteger {
                    overlay_clear();
                    return 0;
                });
            });
            overlay_init();

            //this changes the item array in the config menu :)
            //yes i know it's beautiful you don't have to tell me
            // sq_edit(v, _SC("menu"), [](HSQUIRRELVM v) {
            //     sq_edit(v, _SC("config"), [](HSQUIRRELVM v) {
            //         sq_edit(v, _SC("item"), [](HSQUIRRELVM v) {
            //             sq_pushstring(v, _SC("misc"), -1);
            //             if (SQ_SUCCEEDED(sq_arrayappend(v, -2))) {
            //                 log_printf("Succesfully added to array\n");
            //             }
            //         });
            //     });
            // });

            sq_pop(v, 1);
            return 1;
        }
        return -1;
    }

    dll_export int stdcall release_instance() {
        overlay_destroy();
        config_watcher_stop();
        return 1;
    }

    dll_export int stdcall update_frame() {
        /*
        sq_pushroottable(v);

        //saving ::network.IsPlaying to a variable
        sq_edit(v, _SC("network"), [](HSQUIRRELVM v) {
            SQBool is_playing_sq;
            if (sq_readbool(v, _SC("IsPlaying"), &is_playing_sq)) {
                isplaying = is_playing_sq;
            }
        });

        sq_pop(v, 1);
        */

#if ALLOCATION_PATCH_TYPE != PATCH_NO_ALLOCS
        update_allocs();
#endif

        config_watcher_check();

        return 1;
    }

    dll_export int stdcall render_preproc() {
        return 0;
    }

    dll_export int stdcall render(int, int) {
        overlay_draw();
        return 0;
    }
}
