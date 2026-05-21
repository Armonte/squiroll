#if __INTELLISENSE__
#undef _HAS_CXX20
#define _HAS_CXX20 0
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <windows.h>
#include "log.h"
#include "util.h"
#include "patch_utils.h"


static int cdecl normal_mbox(const char* caption, const UINT type, const char* text) {
    return MessageBoxA(NULL, text, caption, type);
}

mbox_t* log_mbox = &normal_mbox;

// Mirror of stdout to a log file so output survives crashes. log_printf and
// the Squirrel sq_setprintfunc handler both fan out via tee_vprintf below.
// stdout itself is left alone (CONOUT$ from enable_debug_console), so the
// console window keeps showing live output.
FILE* g_log_file = nullptr;

void open_log_file(const char* path) {
    if (g_log_file) return;
    g_log_file = fopen(path, "w");
    if (g_log_file) {
        // Fully buffered, NOT _IONBF. Unbuffered meant every log line
        // was a synchronous disk write — with the per-frame netcode
        // tracing that stalled the game hard. log_flush() drains the
        // buffer once per frame from better_game_loop, so a crash still
        // only loses at most one frame of log.
        setvbuf(g_log_file, nullptr, _IOFBF, 1 << 16);
        fputs("=== squiroll log start ===\n", g_log_file);
    }
    // The console (stdout) tee is just as slow per-call — WriteConsole
    // per line chokes under heavy logging. Fully buffer it too; the
    // per-frame log_flush() keeps the console window ~60 Hz live.
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
}

// Flush both sinks. Call once per frame (better_game_loop), NOT per
// log line — per-line flushing is what made logging a frame-time sink.
void log_flush() {
    if (g_log_file) fflush(g_log_file);
    fflush(stdout);
}

static void tee_vprintf(FILE* stream, const char* format, va_list va) {
    va_list va_copy;
    va_copy(va_copy, va);
    vfprintf(stream, format, va_copy);
    va_end(va_copy);
    if (g_log_file && stream != g_log_file) {
        vfprintf(g_log_file, format, va);
    }
}

extern "C" void cdecl tee_printf(const char* format, ...) {
    va_list va;
    va_start(va, format);
    tee_vprintf(stdout, format, va);
    va_end(va);
}

extern "C" void cdecl tee_fprintf(FILE* stream, const char* format, ...) {
    va_list va;
    va_start(va, format);
    tee_vprintf(stream, format, va);
    va_end(va);
}

// Squirrel sq_setprintfunc handlers — same fan-out, but signature must
// take an opaque VM handle as the first arg, which we ignore. Use void*
// here to avoid pulling in squirrel.h.
extern "C" void sq_print_tee(void*, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    tee_vprintf(stdout, fmt, va);
    va_end(va);
}

extern "C" void sq_error_tee(void*, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    tee_vprintf(stderr, fmt, va);
    va_end(va);
}

#if !DISABLE_ALL_LOGGING_FOR_BUILD

typedef void cdecl vprintf_t(const char* format, va_list va);
typedef void cdecl vfprintf_t(FILE* stream, const char* format, va_list va);

#if !MINGW_COMPAT

printf_t* log_printf = (printf_t*)&printf;
fprintf_t* log_fprintf = (fprintf_t*)&fprintf;

#else

void cdecl printf_lookup(const char* format, ...) {
    printf_t* printf_func = (printf_t*)printf_dummy;
    if (HMODULE msvcrt = GetModuleHandleA("msvcrt.dll")) {
        if (printf_t* log_func = (printf_t*)GetProcAddress(msvcrt, "printf")) {
            printf_func = log_func;
        }
        if (vprintf_t* vprintf_func = (vprintf_t*)GetProcAddress(msvcrt, "vprintf")) {
            va_list va;
            va_start(va, format);
            vprintf_func(format, va);
            va_end(va);
        }
    }
    log_printf = printf_func;
}

void cdecl fprintf_lookup(FILE* stream, const char* format, ...) {
    fprintf_t* fprintf_func = (fprintf_t*)fprintf_dummy;
    if (HMODULE msvcrt = GetModuleHandleA("msvcrt.dll")) {
        if (fprintf_t* log_func = (fprintf_t*)GetProcAddress(msvcrt, "fprintf")) {
            fprintf_func = log_func;
        }
        if (vfprintf_t* vfprintf_func = (vfprintf_t*)GetProcAddress(msvcrt, "vfprintf")) {
            va_list va;
            va_start(va, format);
            vfprintf_func(stream, format, va);
            va_end(va);
        }
    }
    log_fprintf = fprintf_func;
}

printf_t* log_printf = (printf_t*)&printf_lookup;
fprintf_t* log_fprintf = (fprintf_t*)&fprintf_lookup;
#endif

#include <exception>

static constexpr uintptr_t cxx_string_exception_throws[] = {
    0xB910, 0xB977, 0x52993, 0x52A04, 0x83280, 0x832F3, 0x9CD3A, 0x9CEFA, 0x9D0A6,
    0x9D141, 0xC194A, 0xC1B67, 0xE40B0, 0xE4124, 0xE4200, 0xE427E, 0x1233D6, 0x123495
};

typedef void stdcall cxx_throw_exception_string_hook_t(
    msvc_string* str,
    void* throw_info
);

void stdcall cxx_throw_exception_string_hook(
    msvc_string* str,
    void* throw_info
) {
    log_printf(
        "Squirrel C++ exception: \"%s\"\n"
        "Turn on ScrollLock to continue...\n"
        , str->data()
    );
    bool prev_scroll_state = ScrollLockOn();
    SetScrollLockState(false);
    WaitForScrollLock();
    SetScrollLockState(prev_scroll_state);
    return ((cxx_throw_exception_string_hook_t*)(0x2FB5DD_R))(str, throw_info);
}

void patch_throw_logs() {
    nounroll for (size_t i = 0; i < countof(cxx_string_exception_throws); ++i) {
        hotpatch_rel32(based_pointer(base_address, cxx_string_exception_throws[i]), cxx_throw_exception_string_hook);
    }
}

#endif