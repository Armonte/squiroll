#if __INTELLISENSE__
#undef _HAS_CXX20
#define _HAS_CXX20 0
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "log.h"
#include "util.h"
#include "patch_utils.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


static int cdecl normal_mbox(const char* caption, const UINT type, const char* text) {
    return MessageBoxA(NULL, text, caption, type);
}

mbox_t* log_mbox = &normal_mbox;

// ---------------------------------------------------------------------------
// Async logger. The game thread only formats a line and pushes it onto a
// queue; a single background thread does ALL file + console I/O and the
// flushing. Nothing on the game thread ever touches the disk or console —
// that is what used to stall the frame under heavy netcode tracing.
//
// quill was evaluated for this and rejected: it initialises fine, but its
// backend thread abort()s the process inside this game's anti-tamper'd
// 32-bit injected address space (an async fail-fast that bypasses both
// C++ catch and the unhandled-exception filter). A background thread we
// fully control — doing nothing but fwrite — has no such problem.
// ---------------------------------------------------------------------------

FILE* g_log_file = nullptr;

static std::mutex              g_log_mtx;
static std::condition_variable g_log_cv;
static std::deque<std::string> g_log_queue;
static std::atomic<bool>       g_log_running{false};

static void log_worker() {
    std::vector<std::string> batch;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(g_log_mtx);
            g_log_cv.wait(lk, [] { return !g_log_queue.empty() || !g_log_running.load(); });
            if (!g_log_running.load() && g_log_queue.empty()) {
                break;
            }
            while (!g_log_queue.empty()) {
                batch.push_back(std::move(g_log_queue.front()));
                g_log_queue.pop_front();
            }
        }
        for (const std::string& s : batch) {
            if (g_log_file) {
                fwrite(s.data(), 1, s.size(), g_log_file);
            }
            fwrite(s.data(), 1, s.size(), stdout);
        }
        if (g_log_file) {
            fflush(g_log_file);
        }
        fflush(stdout);
        batch.clear();
    }
}

void open_log_file(const char* path) {
    if (g_log_running.load()) return;

    g_log_file = fopen(path, "w");
    if (g_log_file) {
        // Fully buffered — the worker thread fflush()es after each drain,
        // so a crash loses at most one un-drained batch.
        setvbuf(g_log_file, nullptr, _IOFBF, 1 << 16);
    }
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

    g_log_running.store(true);
    std::thread(&log_worker).detach();

    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        g_log_queue.emplace_back("=== squiroll log start ===\n");
    }
    g_log_cv.notify_one();
}

// No-op: the worker thread flushes after every batch on its own. Kept as
// a symbol because better_game_loop calls it once per frame.
void log_flush() {
}

// Format on the calling thread, hand the finished line to the worker.
static void emit_va(const char* format, va_list va) {
    char stackbuf[2048];
    char* buf = stackbuf;
    char* heap = nullptr;

    va_list va2;
    va_copy(va2, va);
    int n = vsnprintf(stackbuf, sizeof(stackbuf), format, va2);
    va_end(va2);
    if (n < 0) return;

    if ((size_t)n >= sizeof(stackbuf)) {
        heap = (char*)malloc((size_t)n + 1);
        if (heap) {
            vsnprintf(heap, (size_t)n + 1, format, va);
            buf = heap;
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        g_log_queue.emplace_back(buf, strlen(buf));
    }
    g_log_cv.notify_one();

    free(heap);
}

extern "C" void cdecl tee_printf(const char* format, ...) {
    va_list va;
    va_start(va, format);
    emit_va(format, va);
    va_end(va);
}

extern "C" void cdecl tee_fprintf(FILE* /*stream*/, const char* format, ...) {
    va_list va;
    va_start(va, format);
    emit_va(format, va);
    va_end(va);
}

// Squirrel sq_setprintfunc handlers — signature takes an opaque VM handle
// first, which we ignore. void* avoids pulling in squirrel.h.
extern "C" void sq_print_tee(void*, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    emit_va(fmt, va);
    va_end(va);
}

extern "C" void sq_error_tee(void*, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    emit_va(fmt, va);
    va_end(va);
}

#if !DISABLE_ALL_LOGGING_FOR_BUILD

static void cdecl async_log_printf(const char* format, ...) {
    va_list va;
    va_start(va, format);
    emit_va(format, va);
    va_end(va);
}

static void cdecl async_log_fprintf(FILE* /*stream*/, const char* format, ...) {
    va_list va;
    va_start(va, format);
    emit_va(format, va);
    va_end(va);
}

printf_t* log_printf = &async_log_printf;
fprintf_t* log_fprintf = &async_log_fprintf;

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
