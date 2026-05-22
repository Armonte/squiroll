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

    // Only genuine fatal faults. Ignore C++ EH (0xE06D7363), debug
    // breakpoints, guard-page hits and the anti-tamper's own exceptions
    // so the log isn't spammed by exceptions the game handles itself.
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Re-entrancy guard: if the handler itself faulted, bail.
    if (InterlockedExchange(&g_in_handler, 1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Flush the async log queue synchronously — the logger's worker
    // thread will not run again, so without this the lines leading up to
    // the crash are lost.
    log_crash_drain();

    const CONTEXT*          c = ep->ContextRecord;
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    char loc[MAX_PATH + 32];

    describe_addr((uintptr_t)r->ExceptionAddress, loc);
    crash_logf("\r\n==== CRASH (squiroll VEH) ====\r\n");
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

    g_in_handler = 0;
    // Observe only — let the exception propagate and crash normally.
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

namespace crash_handler {

void install() {
    if (g_veh) return;
    // Handler-1 = first in the VEH chain, so we log before anything else
    // gets a chance to swallow the exception.
    g_veh = AddVectoredExceptionHandler(1, veh);
    log_printf("crash_handler: VEH %s\n", g_veh ? "installed" : "FAILED");
}

} // namespace crash_handler
