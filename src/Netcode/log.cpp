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

// Pre-allocated fixed-slot ring (no per-line heap allocation, unlike the old
// deque<string>). The game thread only: vsnprintf into a stack buffer, then
// memcpy it into the next ring slot under a (uncontended, brief) lock — no
// malloc, no string ctor. The worker thread drains slots to disk. Modeled on
// the revolve_input_sdl3 ring_log: hot per-frame tracing must not stall the
// frame. If the ring fills (worker behind), lines DROP (counted) rather than
// block the sim thread. Sized for a heavy-trace burst between drains.
static constexpr size_t LINE_MAX = 512;        // per-line cap; longer truncates
static constexpr size_t RING_N   = 32768;      // 32768 * 512 = 16 MB, allocated once
struct LogSlot { uint16_t len; char text[LINE_MAX]; };

static std::mutex              g_log_mtx;
static std::condition_variable g_log_cv;
static LogSlot*                g_ring     = nullptr;
static uint64_t                g_write    = 0;   // total pushed   (under g_log_mtx)
static uint64_t                g_read     = 0;   // total drained  (under g_log_mtx)
static uint64_t                g_dropped  = 0;   // ring-full drops (under g_log_mtx)
static std::atomic<bool>       g_log_running{false};

static inline size_t ring_count() { return (size_t)(g_write - g_read); }

static void log_worker() {
    static std::vector<char> batch;   // reused across drains, no per-drain alloc
    while (true) {
        uint64_t dropped_snapshot = 0;
        {
            std::unique_lock<std::mutex> lk(g_log_mtx);
            g_log_cv.wait(lk, [] { return g_write != g_read || !g_log_running.load(); });
            if (!g_log_running.load() && g_write == g_read) break;
            batch.clear();
            while (g_write != g_read) {
                const LogSlot& s = g_ring[g_read % RING_N];
                batch.insert(batch.end(), s.text, s.text + s.len);
                ++g_read;
            }
            dropped_snapshot = g_dropped; g_dropped = 0;
        }
        if (!batch.empty()) {
            if (g_log_file) fwrite(batch.data(), 1, batch.size(), g_log_file);
            fwrite(batch.data(), 1, batch.size(), stdout);
        }
        if (dropped_snapshot) {
            char w[96];
            int wn = snprintf(w, sizeof w, "[log] !! dropped %llu lines (ring full)\n",
                              (unsigned long long)dropped_snapshot);
            if (g_log_file) fwrite(w, 1, wn, g_log_file);
            fwrite(w, 1, wn, stdout);
        }
        if (g_log_file) fflush(g_log_file);
        fflush(stdout);
    }
}

// Push a finished line into the ring. Drops (counts) if the ring is full so the
// sim thread never blocks on a slow disk. Returns true if the worker should be
// woken (ring was empty).
static inline bool ring_push(const char* buf, size_t n) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    if (ring_count() >= RING_N) { ++g_dropped; return false; }
    bool was_empty = (g_write == g_read);
    LogSlot& s = g_ring[g_write % RING_N];
    memcpy(s.text, buf, n);
    s.len = (uint16_t)n;
    ++g_write;
    return was_empty;
}

void open_log_file(const char* path) {
    if (g_log_running.load()) return;

    if (!g_ring) g_ring = (LogSlot*)malloc(sizeof(LogSlot) * RING_N);

    g_log_file = fopen(path, "w");
    if (g_log_file) {
        // Fully buffered — the worker thread fflush()es after each drain,
        // so a crash loses at most one un-drained batch.
        setvbuf(g_log_file, nullptr, _IOFBF, 1 << 16);
    }
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

    g_log_running.store(true);
    std::thread(&log_worker).detach();

    if (ring_push("=== squiroll log start ===\n", 26)) g_log_cv.notify_one();
}

// No-op: the worker thread flushes after every batch on its own. Kept as
// a symbol because better_game_loop calls it once per frame.
void log_flush() {
}

// Crash-path synchronous drain. The worker thread owns all file I/O; on a
// fatal fault it will not get another chance to run, so flush whatever is
// still in the ring from the faulting thread. Safe to take g_log_mtx here: a
// fault in game code is not holding it. Called from the VEH crash handler.
void log_crash_drain() {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    while (g_write != g_read) {
        const LogSlot& s = g_ring[g_read % RING_N];
        if (g_log_file) fwrite(s.text, 1, s.len, g_log_file);
        fwrite(s.text, 1, s.len, stdout);
        ++g_read;
    }
    if (g_log_file) fflush(g_log_file);
    fflush(stdout);
}

// Format on the calling thread (stack buffer), hand the finished line to the
// ring. No heap allocation; no string construction.
static void emit_va(const char* format, va_list va) {
    if (!g_ring) return;
    char buf[LINE_MAX];
    int n = vsnprintf(buf, sizeof(buf), format, va);
    if (n < 0) return;
    if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;  // truncate over-long lines
    if (ring_push(buf, (size_t)n)) g_log_cv.notify_one();
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
