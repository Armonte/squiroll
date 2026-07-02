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

// Fully LOCK-FREE MPMC ring (Vyukov bounded queue). The game thread NEVER takes a
// lock and NEVER allocates: it CAS-claims a slot, vsnprintf+memcpy into it, and
// publishes via the slot's sequence counter. A single worker thread drains slots
// to disk. This is the same lock-free-ring technique spdlog's async mode uses
// internally — we roll our own because quill/spdlog's backend thread abort()s
// inside th155's anti-tamper'd 32-bit injected address space (the old logger note,
// confirmed). Ring-full -> drop+count, so the sim thread never blocks on slow I/O.
// Heavy per-frame tracing is now ~one CAS + a memcpy on the game thread.
static constexpr size_t   LINE_MAX  = 512;       // per-line cap; longer truncates
static constexpr size_t   RING_N    = 32768;     // power of 2; ~17 MB, allocated once
static constexpr uint64_t RING_MASK = RING_N - 1;
struct LogSlot {
    std::atomic<uint64_t> seq;
    uint16_t              len;
    char                  text[LINE_MAX];
};

static LogSlot*              g_ring = nullptr;
static std::atomic<uint64_t> g_write{0};         // producers CAS this
static std::atomic<uint64_t> g_read{0};          // single consumer (worker)
static std::atomic<uint64_t> g_dropped{0};
static std::atomic<bool>     g_log_running{false};
// Console (stdout) mirror is OFF by default — terminal rendering + fflush per
// drain is the slowest sink and swamps the worker under heavy tracing, dropping
// lines. The file log is the authoritative record. SQUIROLL_LOG_CONSOLE=1 re-enables.
static bool                  g_log_console = false;

// Producer: lock-free claim + publish. No mutex, no allocation.
static inline void ring_push(const char* buf, size_t n) {
    if (!g_ring) return;
    uint64_t pos = g_write.load(std::memory_order_relaxed);
    for (;;) {
        LogSlot& s = g_ring[pos & RING_MASK];
        uint64_t seq = s.seq.load(std::memory_order_acquire);
        int64_t  diff = (int64_t)(seq - pos);
        if (diff == 0) {                         // slot free for sequence `pos`
            if (g_write.compare_exchange_weak(pos, pos + 1,
                                              std::memory_order_relaxed)) {
                memcpy(s.text, buf, n);
                s.len = (uint16_t)n;
                s.seq.store(pos + 1, std::memory_order_release);   // publish
                return;
            }
            // CAS failed: `pos` was reloaded by CAS, retry
        } else if (diff < 0) {                   // ring full (slot not yet freed)
            g_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        } else {                                 // another producer took it
            pos = g_write.load(std::memory_order_relaxed);
        }
    }
}

static void log_worker() {
    static std::vector<char> batch;              // reused; no per-drain alloc
    while (g_log_running.load(std::memory_order_relaxed) ||
           g_read.load(std::memory_order_relaxed) != g_write.load(std::memory_order_relaxed)) {
        batch.clear();
        uint64_t pos = g_read.load(std::memory_order_relaxed);
        for (;;) {                               // drain all published slots
            LogSlot& s = g_ring[pos & RING_MASK];
            uint64_t seq = s.seq.load(std::memory_order_acquire);
            if ((int64_t)(seq - (pos + 1)) != 0) break;   // nothing newer ready
            batch.insert(batch.end(), s.text, s.text + s.len);
            s.seq.store(pos + RING_N, std::memory_order_release);   // free slot
            ++pos;
        }
        g_read.store(pos, std::memory_order_relaxed);

        bool wrote = !batch.empty();
        if (wrote) {
            if (g_log_file) fwrite(batch.data(), 1, batch.size(), g_log_file);
            if (g_log_console) fwrite(batch.data(), 1, batch.size(), stdout);
        }
        uint64_t dropped = g_dropped.exchange(0, std::memory_order_relaxed);
        if (dropped) {
            char w[96];
            int wn = snprintf(w, sizeof w, "[log] !! dropped %llu lines (ring full)\n",
                              (unsigned long long)dropped);
            if (g_log_file) fwrite(w, 1, wn, g_log_file);
            if (g_log_console) fwrite(w, 1, wn, stdout);
            wrote = true;
        }
        if (wrote) { if (g_log_file) fflush(g_log_file); if (g_log_console) fflush(stdout); }
        else       { Sleep(1); }                 // idle: nothing to drain
    }
}

void open_log_file(const char* path) {
    if (g_log_running.load()) return;

    if (!g_ring) {
        g_ring = (LogSlot*)malloc(sizeof(LogSlot) * RING_N);
        for (size_t i = 0; i < RING_N; ++i)
            g_ring[i].seq.store(i, std::memory_order_relaxed);
    }

    g_log_console = (getenv("SQUIROLL_LOG_CONSOLE") != nullptr);
    g_log_file = fopen(path, "w");
    if (g_log_file) {
        // Fully buffered — the worker thread fflush()es after each drain,
        // so a crash loses at most one un-drained batch.
        setvbuf(g_log_file, nullptr, _IOFBF, 1 << 16);
    }
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

    g_log_running.store(true);
    std::thread(&log_worker).detach();

    ring_push("=== squiroll log start ===\n", 26);
}

// No-op: the worker thread flushes after every batch on its own. Kept as
// a symbol because better_game_loop calls it once per frame.
void log_flush() {
}

// Crash-path drain. On a fatal fault the worker may not run again; best-effort
// drain whatever is published from the faulting thread. Lock-free (racy with the
// worker, but the worst case at a crash is a torn tail line — acceptable).
void log_crash_drain() {
    if (!g_ring) return;
    uint64_t pos = g_read.load(std::memory_order_relaxed);
    uint64_t w   = g_write.load(std::memory_order_relaxed);
    for (; pos != w; ++pos) {
        LogSlot& s = g_ring[pos & RING_MASK];
        uint64_t seq = s.seq.load(std::memory_order_acquire);
        if ((int64_t)(seq - (pos + 1)) != 0) continue;   // not published
        if (g_log_file) fwrite(s.text, 1, s.len, g_log_file);
        fwrite(s.text, 1, s.len, stdout);
    }
    if (g_log_file) fflush(g_log_file);
    fflush(stdout);
}

// Format on the calling thread (stack buffer), publish to the lock-free ring.
static void emit_va(const char* format, va_list va) {
    char buf[LINE_MAX];
    int n = vsnprintf(buf, sizeof(buf), format, va);
    if (n < 0) return;
    if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;   // truncate over-long lines
    ring_push(buf, (size_t)n);
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
