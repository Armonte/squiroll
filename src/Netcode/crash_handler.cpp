#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>

#include "crash_handler.h"
#include "log.h"

// Vectored exception handler — see crash_handler.h. Everything here must be
// crash-safe: no CRT locks, no heap, no float. wvsprintfA is used instead of
// snprintf because it touches none of those (it also has no %f, which we
// don't need). Output goes to aocf_crash.log via raw WriteFile.

namespace {

static volatile LONG g_in_handler = 0;
static PVOID         g_veh        = nullptr;
static volatile LONG g_watch_cxx  = 0;   // log first-chance C++ exceptions
static volatile LONG g_cxx_quota  = 12;  // ...but only this many, no spam

// Hardware data-write watchpoint (DR0): when armed, every write to g_wp_addr
// raises a single-step exception the VEH logs (the writing instruction's
// EIP + the new value). Used to find which code writes a rollback-divergent
// field. Armed/disarmed on the simulation thread (debug registers are
// per-thread), scoped to the frame under investigation.
static void*         g_wp_addr  = nullptr;
static volatile LONG g_wp_quota = 0;
static uint32_t      g_wp_last  = 0;     // last logged value (log on change)

// All th155 worker threads (registered from the _beginthreadex hook), so a
// hardware watchpoint can be armed on EVERY thread — not just the sim
// thread — to catch a non-sim writer.
static DWORD            g_tids[64] = {0};
static int              g_n_tids   = 0;
static CRITICAL_SECTION g_tids_lock;

// Append a chunk to the crash log fully synchronously.
static void crash_write(const char* data, int len) {
    if (len <= 0) return;
    HANDLE h = CreateFileA("aocf_crash.log", FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD wrote = 0;
    WriteFile(h, data, (DWORD)len, &wrote, nullptr);
    CloseHandle(h);
}

static void crash_logf(const char* fmt, ...) {
    char buf[1024];
    va_list va;
    va_start(va, fmt);
    int n = wvsprintfA(buf, fmt, va);
    va_end(va);
    crash_write(buf, n);
    OutputDebugStringA(buf);
}

// Resolve a runtime address to "module+0xRVA". The RVA maps straight into
// the IDA db (th155.exe IDB is based at 0, so RVA == IDA address).
static void describe_addr(uintptr_t addr, char* out) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) &&
        mod) {
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = path;
        for (const char* p = path; *p; ++p) {
            if (*p == '\\' || *p == '/') base = p + 1;
        }
        wsprintfA(out, "%s+0x%X", base, (unsigned)(addr - (uintptr_t)mod));
    } else {
        wsprintfA(out, "0x%08X <no module>", (unsigned)addr);
    }
}

static LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;

    // Hardware data-write watchpoint hit: a DR register fired a single-step.
    // Log the writing instruction (EIP is the instruction AFTER the write)
    // and the value now at the address, then continue.
    if (code == (DWORD)EXCEPTION_SINGLE_STEP) {
        if (g_wp_addr && (ep->ContextRecord->Dr6 & 0xFu)) {
            uint32_t v = *(volatile uint32_t*)g_wp_addr;
            if (v != g_wp_last && g_wp_quota > 0) {
                InterlockedDecrement(&g_wp_quota);
                g_wp_last = v;
                char loc[MAX_PATH + 32];
                describe_addr(ep->ContextRecord->Eip, loc);
                log_printf("[wp] %p <- %08X  eip=%08X tid=%u %s\n",
                           g_wp_addr, v, (unsigned)ep->ContextRecord->Eip,
                           GetCurrentThreadId(), loc);
            }
            ep->ContextRecord->Dr6 = 0;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Genuine fatal faults are always logged. Any OTHER exception code — C++
    // EH (0xE06D7363), heap corruption (0xC0000374), ... — is normally
    // ignored (the game raises/handles its own), but while watch_cxx is on (a
    // rollback re-sim advance) a bounded number are logged. This catches a
    // re-sim crash whose code is outside the always-fatal set.
    bool watched = false;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        break;
    default:
        if (!g_watch_cxx || g_cxx_quota <= 0)
            return EXCEPTION_CONTINUE_SEARCH;
        InterlockedDecrement(&g_cxx_quota);
        watched = true;
        break;
    }

    // Re-entrancy guard: if the handler itself faulted, bail.
    if (InterlockedExchange(&g_in_handler, 1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const CONTEXT*          c = ep->ContextRecord;
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    char loc[MAX_PATH + 32];

    describe_addr((uintptr_t)r->ExceptionAddress, loc);
    crash_logf(watched ? "\r\n==== FIRST-CHANCE EXCEPTION (re-sim) ====\r\n"
                       : "\r\n==== CRASH (squiroll VEH) ====\r\n");
    crash_logf("code=0x%08X  at %s  eip=0x%08X\r\n", code, loc, c->Eip);
    if (code == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        const ULONG_PTR kind = r->ExceptionInformation[0];
        const char* op = kind == 0 ? "READ" : kind == 1 ? "WRITE" : "EXEC";
        crash_logf("  %s of 0x%08X\r\n", op,
                   (unsigned)r->ExceptionInformation[1]);
    }
    crash_logf("  eax=%08X ebx=%08X ecx=%08X edx=%08X\r\n",
               c->Eax, c->Ebx, c->Ecx, c->Edx);
    crash_logf("  esi=%08X edi=%08X ebp=%08X esp=%08X\r\n",
               c->Esi, c->Edi, c->Ebp, c->Esp);

    // EBP-chain stack walk — resolves each return address to module+RVA.
    crash_logf("  --- stack (ebp chain) ---\r\n");
    uintptr_t ebp = c->Ebp;
    for (int i = 0; i < 32 && ebp; ++i) {
        if (IsBadReadPtr((void*)ebp, 8)) break;
        const uintptr_t ret  = *(uintptr_t*)(ebp + 4);
        const uintptr_t next = *(uintptr_t*)ebp;
        if (ret) {
            describe_addr(ret, loc);
            crash_logf("  [%2d] ret=0x%08X  %s\r\n", i, (unsigned)ret, loc);
        }
        if (next <= ebp) break;  // frames must climb the stack
        ebp = next;
    }
    crash_logf("==== END CRASH ====\r\n");

    // Drain the async log queue AFTER the crash dump is safely on disk: if
    // the heap is corrupted the drain itself can fault, and we must not
    // lose the dump to a re-entrant crash. A watched first-chance exception
    // may be non-fatal (the program continues), so don't drain it.
    if (!watched) log_crash_drain();

    g_in_handler = 0;
    if (!watched) {
        // Real fatal fault. Self-terminate instead of letting it propagate:
        // the Windows error dialog freezes every thread (including the
        // logger) and hangs the process. The crash dump is already on disk.
        TerminateProcess(GetCurrentProcess(), code);
    }
    // Watched first-chance exception — observe only, let it propagate.
    return EXCEPTION_CONTINUE_SEARCH;
}

// --- fast-fail interception -------------------------------------------------
// __fastfail (int 29h) bypasses VEH/UEF entirely — a /GS stack-cookie smash or
// a CRT invalid-parameter kills the process with no crash report. We hook the
// th155 CRT functions that issue it; each runs with the stack still intact, so
// the hook logs an EBP-chain walk pinpointing the faulting function, then ends
// the process cleanly.
static SafetyHookInline g_ff[5];

static void log_fastfail_stack(const char* via) {
    log_crash_drain();
    crash_logf("\r\n==== FASTFAIL via %s ====\r\n", via);
    uintptr_t ebp = (uintptr_t)__builtin_frame_address(0);
    char loc[MAX_PATH + 32];
    for (int i = 0; i < 48 && ebp; ++i) {
        if (IsBadReadPtr((void*)ebp, 8)) break;
        const uintptr_t ret  = *(uintptr_t*)(ebp + 4);
        const uintptr_t next = *(uintptr_t*)ebp;
        if (ret) {
            describe_addr(ret, loc);
            crash_logf("  [%2d] ret=0x%08X  %s\r\n", i, (unsigned)ret, loc);
        }
        if (next <= ebp) break;
        ebp = next;
    }
    crash_logf("==== END FASTFAIL ====\r\n");
}

#define FF_HOOK(idx, nm)                              \
    static void __cdecl ff_hook_##idx() {             \
        log_fastfail_stack(nm);                       \
        TerminateProcess(GetCurrentProcess(), 0xC0000409u); \
    }
FF_HOOK(0, "__report_gsfailure")
FF_HOOK(1, "__report_securityfailure")
FF_HOOK(2, "__scrt_fastfail")
FF_HOOK(3, "__invoke_watson")
FF_HOOK(4, "abort")

// --- process-exit interception ----------------------------------------------
// If the crash is not an exception at all — a clean ExitProcess / Terminate-
// Process / RaiseFailFastException from some th155 error path — these catch it
// with the call stack intact.
static SafetyHookInline g_h_exit{}, g_h_term{}, g_h_raiseff{};

static void __stdcall hook_exitprocess(UINT code) {
    log_fastfail_stack("ExitProcess");
    crash_logf("  exit code = 0x%08X\r\n", code);
    g_h_exit.unsafe_stdcall<void>(code);
}
static BOOL __stdcall hook_terminateprocess(HANDLE h, UINT code) {
    if (h == GetCurrentProcess() || h == (HANDLE)(LONG_PTR)-1) {
        log_fastfail_stack("TerminateProcess");
        crash_logf("  exit code = 0x%08X\r\n", code);
    }
    return g_h_term.unsafe_stdcall<BOOL>(h, code);
}
static void __stdcall hook_raiseff(void* rec, void* ctx, DWORD flags) {
    log_fastfail_stack("RaiseFailFastException");
    g_h_raiseff.unsafe_stdcall<void>(rec, ctx, flags);
}

} // namespace

namespace crash_handler {

void watch_cxx(bool on) { g_watch_cxx = on ? 1 : 0; }

// Arm a hardware data-write watchpoint on `addr` (4 bytes) for the CURRENT
// thread — call this on the simulation thread. Every write to those bytes
// then traps into veh(), which logs the writing instruction.
// Set DR0 watchpoint on a non-current th155 thread (Suspend / Get/SetThread-
// Context / Resume). addr=0 disarms.
static void wp_apply_other(DWORD tid, void* addr) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                          THREAD_SET_CONTEXT, FALSE, tid);
    if (!h) return;
    if (SuspendThread(h) != (DWORD)-1) {
        CONTEXT ctx; ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &ctx)) {
            ctx.Dr0 = (DWORD)(uintptr_t)addr;
            ctx.Dr6 = 0;
            ctx.Dr7 = (ctx.Dr7 & ~0x000F0003u) |
                      (addr ? 0x000D0001u : 0u);
            SetThreadContext(h, &ctx);
        }
        ResumeThread(h);
    }
    CloseHandle(h);
}

void register_thread(uint32_t tid) {
    EnterCriticalSection(&g_tids_lock);
    bool exists = false;
    for (int i = 0; i < g_n_tids; ++i)
        if (g_tids[i] == (DWORD)tid) { exists = true; break; }
    if (!exists && g_n_tids < 64) g_tids[g_n_tids++] = (DWORD)tid;
    LeaveCriticalSection(&g_tids_lock);
    // If a watchpoint is already armed, propagate it onto the new thread so
    // it's covered from the instant of its first instruction.
    if (g_wp_addr && (DWORD)tid != GetCurrentThreadId())
        wp_apply_other((DWORD)tid, g_wp_addr);
}

void watchpoint_arm(void* addr) {
    g_wp_addr  = addr;
    g_wp_quota = 256;
    g_wp_last  = *(volatile uint32_t*)addr;
    // Self
    CONTEXT ctx; ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE self = GetCurrentThread();
    if (GetThreadContext(self, &ctx)) {
        ctx.Dr0 = (DWORD)(uintptr_t)addr;
        ctx.Dr6 = 0;
        // DR7: L0=1; R/W0 (bits 16-17)=01 write; LEN0 (bits 18-19)=11 4 bytes.
        ctx.Dr7 = (ctx.Dr7 & ~0x000F0003u) | 0x000D0001u;
        SetThreadContext(self, &ctx);
    }
    // Fan out to every other registered th155 thread so a non-sim writer
    // (audio / input polling / loader) is caught too.
    DWORD me = GetCurrentThreadId();
    int n = 0;
    EnterCriticalSection(&g_tids_lock);
    for (int i = 0; i < g_n_tids; ++i)
        if (g_tids[i] != me) { wp_apply_other(g_tids[i], addr); ++n; }
    int total = g_n_tids;
    LeaveCriticalSection(&g_tids_lock);
    log_printf("[wp] armed on %p (self tid=%u + %d/%d other th155 threads)\n",
               addr, me, n, total);
}

void watchpoint_disarm() {
    CONTEXT ctx; ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE self = GetCurrentThread();
    if (GetThreadContext(self, &ctx)) {
        ctx.Dr0 = 0;
        ctx.Dr7 &= ~0x000F0003u;
        SetThreadContext(self, &ctx);
    }
    DWORD me = GetCurrentThreadId();
    EnterCriticalSection(&g_tids_lock);
    for (int i = 0; i < g_n_tids; ++i)
        if (g_tids[i] != me) wp_apply_other(g_tids[i], nullptr);
    LeaveCriticalSection(&g_tids_lock);
    g_wp_addr = nullptr;
}

void install() {
    if (g_veh) return;
    InitializeCriticalSection(&g_tids_lock);
    // Handler-1 = first in the VEH chain, so we log before anything else
    // gets a chance to swallow the exception.
    g_veh = AddVectoredExceptionHandler(1, veh);
    log_printf("crash_handler: VEH %s\n", g_veh ? "installed" : "FAILED");

    // Hook th155's __fastfail issuers (RVAs from the IDB; th155.exe base 0).
    static const uint32_t ff_rva[5] = {
        0x2e1c27, 0x2e1d2e, 0x2e2788, 0x306f89, 0x30c5d7 };
    void* ff_repl[5] = { (void*)ff_hook_0, (void*)ff_hook_1, (void*)ff_hook_2,
                         (void*)ff_hook_3, (void*)ff_hook_4 };
    uint8_t* base = (uint8_t*)GetModuleHandleA(nullptr);
    int ffok = 0;
    for (int i = 0; i < 5; ++i) {
        g_ff[i] = safetyhook::create_inline(base + ff_rva[i], ff_repl[i]);
        ffok += g_ff[i].enabled() ? 1 : 0;
    }
    log_printf("crash_handler: fastfail hooks %d/5\n", ffok);

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        void* pe = (void*)GetProcAddress(k32, "ExitProcess");
        void* pt = (void*)GetProcAddress(k32, "TerminateProcess");
        void* pr = (void*)GetProcAddress(k32, "RaiseFailFastException");
        if (pe) g_h_exit    = safetyhook::create_inline(pe, (void*)hook_exitprocess);
        if (pt) g_h_term    = safetyhook::create_inline(pt, (void*)hook_terminateprocess);
        if (pr) g_h_raiseff = safetyhook::create_inline(pr, (void*)hook_raiseff);
        log_printf("crash_handler: exit hooks exit=%d term=%d raiseff=%d\n",
                   (int)g_h_exit.enabled(), (int)g_h_term.enabled(),
                   (int)g_h_raiseff.enabled());
    }
}

} // namespace crash_handler
