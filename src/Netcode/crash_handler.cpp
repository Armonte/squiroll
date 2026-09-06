#include <safetyhook.hpp>

#include <windows.h>
#include <tlhelp32.h>   // dump_all_thread_stacks (hang watchdog)
#include <stdint.h>

#include "crash_handler.h"
#include "log.h"
#include "util.h"      // base_address — needed by the clguard VEH path
#include "cpp_arena.h" // describe_block — crash-time arena attribution

// Zydis — used by the universal NULL-deref skip in the VEH.
#define ZYAN_NO_LIBC
#include <Zydis/Zydis.h>

// Vectored exception handler — see crash_handler.h. Everything here must be
// crash-safe: no CRT locks, no heap, no float. wvsprintfA is used instead of
// snprintf because it touches none of those (it also has no %f, which we
// don't need). Output goes to aocf_crash.log via raw WriteFile.

namespace gekko_bridge { extern int g_trace_frame; extern int g_trace_rb; }

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

// Scan `ndw` dwords upward from `sp` for th155 code addresses and log them.
// Used for the CRASHED stack (VEH) and the fast-fail issuer's caller stack:
// th155 is FPO'd, so the ebp chain hides every game frame; the raw scan is
// the only way to see which th155 function actually issued the abort/fault.
// clguard RECOVERY GATE (default OFF as of 2026-09-06).
//
// The two "recoveries" below (skip a NULL-deref instruction; return from an
// EXEC-at-NULL by popping the return address) were added to keep a run alive past
// a fault so a later symptom could be observed. They are ACTIVELY HARMFUL as a
// default:
//   * EXEC-at-NULL pops ONLY the return address. Every th155 vtable slot it fires
//     on is __stdcall/__thiscall — the callee is supposed to clean its arguments —
//     so the caller resumes with a shifted stack. That is exactly how the missing
//     _Delete_this vtable slot (fixed in 7d77cc3) turned into "0xC0000374 heap
//     corruption" three frames later instead of a clean fault at the real site.
//   * The universal NULL-deref skip leaves the destination register holding a
//     stale value and lets the game run on with garbage.
// Both convert a precise, attributable crash into a corrupted process that dies
// somewhere unrelated. Default: log the fault in full and let it be fatal, which
// is what makes a bug findable. SQUIROLL_CLGUARD_RECOVER=1 restores the old
// keep-running behaviour for the rare case where surviving the fault is the point.
static bool clguard_recover_enabled() {
    static int on = -1;
    if (on < 0) {
        char b[8] = {0};
        DWORD n = GetEnvironmentVariableA("SQUIROLL_CLGUARD_RECOVER", b, sizeof b);
        on = (n > 0 && b[0] == '1') ? 1 : 0;
    }
    return on != 0;
}

static void scan_rets(const uint32_t* sp, int ndw, int maxshow, const char* indent) {
    char loc[MAX_PATH + 32];
    for (int k = 0, shown = 0; k < ndw && shown < maxshow; ++k) {
        MEMORY_BASIC_INFORMATION m;
        if (VirtualQuery((const void*)&sp[k], &m, sizeof m) != sizeof m ||
            m.State != MEM_COMMIT || (m.Protect & PAGE_GUARD) || m.Protect == PAGE_NOACCESS)
            break;
        const uint32_t v = sp[k];
        const uint32_t rva = v - (uint32_t)::base_address;
        if (rva >= 0x1000 && rva < 0x300000) {
            describe_addr(v, loc);
            crash_logf("%ssp[+0x%03X] rva=%08X  %s\r\n", indent, k * 4, rva, loc);
            ++shown;
        }
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
            // SQUIROLL_WP_NONIMG=1: log ONLY writes whose new value is OUTSIDE
            // the th155 image. For a vtable-dword watch this silences the
            // legitimate traffic (ctor/dtor vtable stores + restore memcpys all
            // write image addresses — thousands of hits across a match) and
            // fires solely on the CORRUPTING write (a heap value smeared over
            // the vtable — the 0xEAC9 round-end crash writer).
            static int nonimg = -1;
            if (nonimg < 0) { char b[4] = {0};
                nonimg = (GetEnvironmentVariableA("SQUIROLL_WP_NONIMG", b, sizeof b) > 0
                          && b[0] != '0') ? 1 : 0; }
            if (nonimg) {
                uint32_t lo = (uint32_t)base_address + 0x1000;
                uint32_t hi = (uint32_t)base_address + 0x500000;
                if (v >= lo && v < hi) {           // legit image-range value
                    ep->ContextRecord->Dr6 = 0;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
            // Log every write while quota lasts. The previous \"only when v
            // changed\" filter hid the case where a re-sim writer writes the
            // same value repeatedly — exactly the f=15 sq-arena pattern we
            // need to attribute (forward writes 3, re-sims deterministically
            // write 2; subsequent re-sim writers all write 2 == g_wp_last
            // and never logged). Quota still bounds total log volume.
            if (g_wp_quota > 0) {
                InterlockedDecrement(&g_wp_quota);
                g_wp_last = v;
                char loc[MAX_PATH + 32];
                describe_addr(ep->ContextRecord->Eip, loc);
                log_printf("[wp] %p <- %08X  eip=%08X tid=%u %s\n",
                           g_wp_addr, v, (unsigned)ep->ContextRecord->Eip,
                           GetCurrentThreadId(), loc);
                // Walk a few stack frames so the caller chain is visible.
                uintptr_t ebp = ep->ContextRecord->Ebp;
                for (int i = 0; i < 6 && ebp; ++i) {
                    if (IsBadReadPtr((void*)ebp, 8)) break;
                    uintptr_t ret  = *(uintptr_t*)(ebp + 4);
                    uintptr_t next = *(uintptr_t*)ebp;
                    if (ret) {
                        describe_addr(ret, loc);
                        log_printf("[wp]   stk[%d] ret=%08X %s\n", i, (unsigned)ret, loc);
                    }
                    if (next <= ebp) break;
                    ebp = next;
                }
            }
            ep->ContextRecord->Dr6 = 0;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // DEFENSIVE GUARD: concurrent_list_walk_visit (th155 0x13B00) dereferences
    // each list node's payload pointer at two sites — and on hit, the game's
    // task list sometimes contains a stale entry whose payload field has been
    // overwritten by a small int (~0x870000). We need the game to keep
    // running so we can hunt the corruptor's source, so absorb the AV and
    // step past the bad instruction with cleared / set flags that take the
    // \"skip this node\" branch.
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        const CONTEXT* gc = ep->ContextRecord;
        uint32_t rva = (uint32_t)((uintptr_t)ep->ExceptionRecord->ExceptionAddress
                                  - ::base_address);
        // Three crash sites in concurrent_list_walk_visit (th155 0x13B00).
        // ALL go through the same recovery: jump to the function epilogue
        // at 0x13B86 (pop edi -> pop esi -> pop ebx -> mov esp,ebp ->
        // pop ebp -> retn 10h). That bypasses the `mov [edi+8], esi`
        // store at 0x13B83 so we don't corrupt list_state[2] further.
        // mov esp,ebp normalizes the stack, so jumping from anywhere
        // inside the function body lands safely.
        if (rva == 0x13B31 || rva == 0x13B34 || rva == 0x13B5E) {
            static uint32_t total_hits = 0;
            ++total_hits;
            if ((total_hits & 0xFF) == 1) {
                crash_logf("\r\n[clguard] AV at 0x%X in walk_visit "
                           "(esi=%08X edi=%08X) — jumping to epilogue. "
                           "(total hits #%u)\r\n",
                           rva, gc->Esi, gc->Edi, total_hits);
            }

            // First crash at 0x13B34 only: arm DR0 on the list_state+8
            // slot once so we can attribute the upstream corruptor's
            // write. list_state was saved at [ebp-4] by the prologue.
            if (rva == 0x13B34) {
                uintptr_t list_state = 0;
                if (!IsBadReadPtr((void*)(uintptr_t)(gc->Ebp - 4), 4)) {
                    list_state = *(uintptr_t*)(uintptr_t)(gc->Ebp - 4);
                }
                static bool g_clguard_armed = false;
                if (!g_clguard_armed && list_state) {
                    void* slot = (void*)(list_state + 8);
                    g_wp_addr  = slot;
                    g_wp_quota = 256;
                    g_wp_last  = *(volatile uint32_t*)slot;
                    CONTEXT* hctx = ep->ContextRecord;
                    hctx->Dr0 = (DWORD)(uintptr_t)slot;
                    hctx->Dr6 = 0;
                    hctx->Dr7 = (hctx->Dr7 & ~0x000F0003u) | 0x000D0001u;
                    crash_logf("[clguard] DR0 armed on list_state+8 slot "
                               "%08X (current=%08X) — next write to this "
                               "slot logs the corruptor\r\n",
                               (unsigned)(uintptr_t)slot, g_wp_last);
                    g_clguard_armed = true;
                }
            }

            // Jump straight to the function epilogue. Bypass the bad
            // store at 0x13B83 — we leave list_state[2] untouched so
            // the corruptor's bogus value is preserved (and the next
            // walk just re-fires our guard).
            ep->ContextRecord->Eip = ::base_address + 0x13B86;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        // UNIVERSAL NULL-DEREF SKIP. Once cpp_arena leak-on-free is on,
        // every post-UAF crash is a deref of a zeroed object: read or
        // write at a low (< 0x10000) address. Skip the offending
        // instruction wholesale: decode it with Zydis, advance EIP by
        // its length, and continue. The destination register keeps
        // its prior value — which is acceptable downstream because the
        // higher-level th155 logic ALREADY tolerates the deref's result
        // (the only path that should write to a freed object SHOULD
        // have checked the object's validity, but didn't; the rest of
        // the function expects a sane return).
        //
        // Special case: EIP is at NULL itself (the AV's fault address
        // is the EIP we're trying to execute). That happens after a
        // `call eax` where eax=0 — the call pushed the return address
        // and then jumped to 0. We simulate the called function
        // returning immediately: pop the return address off the stack
        // and resume there. eax is left at whatever value it had
        // (which is 0 anyway), and caller-saved regs are clobbered the
        // same way a no-op function would clobber them.
        if (ep->ExceptionRecord->NumberParameters >= 2 &&
            gc->Eip < 0x10000u) {
            // EXEC-at-NULL recovery — only for AVs where EIP=fault_addr
            // and both are in the NULL page.
            uintptr_t fault_addr =
                (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
            if (fault_addr == (uintptr_t)gc->Eip) {
                static uint32_t hits = 0;
                ++hits;
                if ((hits & 0x7F) == 1) {
                    crash_logf("\r\n[clguard] EXEC-at-NULL recovery: "
                               "eip=%08X esp=%08X (popping return addr). "
                               "(hit #%u)\r\n",
                               gc->Eip, gc->Esp, hits);
                    crash_logf("  f=%d rb=%d\r\n", gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb);
                    scan_rets((const uint32_t*)(uintptr_t)gc->Esp, 24, 8, "    ");
                }
                if (clguard_recover_enabled() &&
                    !IsBadReadPtr((void*)(uintptr_t)gc->Esp, 4)) {
                    uintptr_t ret_addr = *(uintptr_t*)(uintptr_t)gc->Esp;
                    ep->ContextRecord->Eip = (DWORD)ret_addr;
                    ep->ContextRecord->Esp += 4;
                    ep->ContextRecord->Eax = 0;  // dtor-style "void"
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                // Not recovering: fall through to the full crash dump. A call
                // through a NULL vtable slot is a REAL bug at a KNOWN site —
                // the stack scan above already named the caller.
            }
        }
        if (ep->ExceptionRecord->NumberParameters >= 2) {
            uintptr_t fault_addr =
                (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
            if (fault_addr < 0x500000u && gc->Eip >= 0x10000u) {
                // Decode the faulting instruction so we can step over it.
                ZydisDecoder dec;
                ZydisDecoderInit(&dec,
                                 ZYDIS_MACHINE_MODE_LEGACY_32,
                                 ZYDIS_STACK_WIDTH_32);
                ZydisDecodedInstruction insn;
                ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
                ZyanStatus zs = ZydisDecoderDecodeFull(
                    &dec,
                    (const void*)(uintptr_t)gc->Eip,
                    16, &insn, ops);
                if (ZYAN_SUCCESS(zs)) {
                    static uint32_t hits = 0;
                    ++hits;
                    if ((hits & 0x7F) == 1) {
                        crash_logf("\r\n[clguard] universal NULL-deref skip: "
                                   "eip=%08X rva=%05X fault=%08X insn_len=%u "
                                   "(hit #%u)\r\n",
                                   gc->Eip, rva, (unsigned)fault_addr,
                                   (unsigned)insn.length, hits);
                        crash_logf("  f=%d rb=%d ecx=%08X edi=%08X esi=%08X\r\n",
                                   gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb,
                                   gc->Ecx, gc->Edi, gc->Esi);
                        scan_rets((const uint32_t*)(uintptr_t)gc->Esp, 32, 8, "    ");
                    }
                    if (clguard_recover_enabled()) {
                        ep->ContextRecord->Eip += insn.length;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                    // else: fall through and report the fault where it happened.
                }
            }
        }

        // sub_EC130 at 0xEC1CD: `call dword ptr [eax]` where eax = NULL
        // because we zero-filled a freed object in cpp_arena. The
        // surrounding code does `if (edx)` to NULL-check the object
        // itself, but not its vtable pointer. With leak-on-free zeroing
        // the freed block, edx is non-null (points at the zeroed slot)
        // and [edx] = 0, so [eax] = [0] = AV. Skip the call to the
        // natural continuation at 0xEC1CF.
        if (rva == 0xEC1CD) {
            static uint32_t hits = 0;
            ++hits;
            if ((hits & 0x7F) == 1) {
                crash_logf("\r\n[clguard] AV at 0xEC1CD (call [eax]) "
                           "eax=%08X edx=%08X — skipping bad dtor call. "
                           "(hit #%u)\r\n",
                           gc->Eax, gc->Edx, hits);
            }
            ep->ContextRecord->Eip = ::base_address + 0xEC1CF;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        // Manbow::Actor2DGroup::RebuildActorList (th155 0x9B1A0) crash
        // site at 0x9B1AB: `mov [eax], edi` where eax is the current
        // node's prev pointer — corrupted to a th155 .rdata address.
        // The same pointer-corruption pattern as walk_visit; here it
        // shows up during the unlink-bad-entry path.
        //
        // Recovery: skip past the unlink + dtor + free, jump to the
        // \"esi = edi (next)\" advance at 0x9B1EF. The list keeps the
        // bad node in place, but iteration continues to the next
        // node so the rebuild doesn't abort the frame.
        if (rva == 0x9B1AB || rva == 0x9B1B2) {
            static uint32_t hits = 0;
            ++hits;
            if ((hits & 0xFF) == 1) {
                crash_logf("\r\n[clguard] AV at 0x%X in RebuildActorList "
                           "(eax=%08X esi=%08X) — skipping unlink, "
                           "advancing to next node. (hit #%u)\r\n",
                           rva, gc->Eax, gc->Esi, hits);
            }
            // edi already holds `next` (from `mov edi, [esi]` at 0x9B1A9).
            // Jump to `mov esi, edi` at 0x9B1EF.
            ep->ContextRecord->Eip = ::base_address + 0x9B1EF;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
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
    crash_logf("  f=%d rb=%d\r\n", gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb);
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
    // Arena attribution: any register pointing into an allocated cpp-arena
    // block gets its allocator call-site RVA from the block header — tells us
    // WHAT object (by construction site) is involved in the crash.
    {
        struct { const char* n; DWORD v; } regs[] = {
            {"eax", c->Eax}, {"ebx", c->Ebx}, {"ecx", c->Ecx}, {"edx", c->Edx},
            {"esi", c->Esi}, {"edi", c->Edi},
        };
        for (auto& rg : regs) {
            uint32_t rva = 0, sz = 0, pay = 0;
            if (cpp_arena::describe_block(rg.v, &rva, &sz, &pay))
                crash_logf("  %s -> arena block payload=%08X size=%u alloc_rva=%08X\r\n",
                           rg.n, pay, sz, rva);
        }
    }

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
    // EXEC-AT-HEAP diagnosis (task #28 render crash): when EIP is in no module
    // (executing heap because a vtable/fptr was corrupted), the ebp chain is
    // all-driver and useless. Scan the CRASHED stack (gc->Esp) for th155
    // return addresses to recover the game render call site, AND probe the
    // registers/stack for the object whose vtable points into heap (the freed
    // render/effect object). Also dump the bytes at EIP (the fake "code" = the
    // object's overwritten first dword) so we can see what clobbered it.
    {
        bool in_image = (c->Eip >= ::base_address + 0x1000 &&
                         c->Eip <  ::base_address + 0x300000);
        if (!in_image) {
            crash_logf("  --- exec-at-heap: th155 render call site scan ---\r\n");
            const uint32_t* sp = (const uint32_t*)(uintptr_t)c->Esp;
            for (int k = 0, shown = 0; k < 256 && shown < 16; ++k) {
                if (IsBadReadPtr((void*)&sp[k], 4)) break;
                uint32_t v = sp[k];
                uint32_t rva = v - (uint32_t)::base_address;
                if (rva >= 0x1000 && rva < 0x300000) {
                    describe_addr(v, loc);
                    crash_logf("    esp[+0x%03X] rva=%08X  %s\r\n",
                               k * 4, rva, loc);
                    ++shown;
                }
            }
            // The corrupted object: EIP is its clobbered vtable[0] target;
            // dump 16 bytes at EIP (the object's first bytes) + the regs that
            // likely hold the object pointer.
            if (!IsBadReadPtr((void*)(uintptr_t)c->Eip, 16)) {
                const uint8_t* p = (const uint8_t*)(uintptr_t)c->Eip;
                crash_logf("  obj-bytes @EIP: %02X %02X %02X %02X %02X %02X %02X "
                           "%02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                           p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
                           p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
            }
            crash_logf("  regs: eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
                       "ebx=%08X\r\n", c->Eax, c->Ecx, c->Edx, c->Esi,
                       c->Edi, c->Ebx);
        }
    }
    crash_logf("  --- raw stack scan (th155 ret addrs, crashed esp) ---\r\n");
    scan_rets((const uint32_t*)(uintptr_t)c->Esp, 160, 24, "    ");
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

static void log_fastfail_stack(const char* via, const uint32_t* sp0 = nullptr) {
    log_crash_drain();
    crash_logf("\r\n==== FASTFAIL via %s ====\r\n", via);
    crash_logf("  f=%d rb=%d\r\n", gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb);
    if (sp0) {
        // sp0 = the hooked issuer's return-address slot: sp0[0] is the exact
        // th155 call site of abort()/fastfail, sp0[1..] its caller's stack.
        crash_logf("  --- issuer call site + caller stack ---\r\n");
        scan_rets(sp0, 40, 12, "    ");
    }
    // NB: do NOT probe with IsBadReadPtr here — it works by raising a real AV
    // and swallowing it in SEH, but our VEH runs FIRST and logs it as a
    // "CRASH at KERNEL32+..." (polluting exit-path logs and misclassifying
    // clean runs as crashes). VirtualQuery bounds raise no exceptions.
    auto readable = [](uintptr_t p, size_t len) -> bool {
        MEMORY_BASIC_INFORMATION m;
        if (VirtualQuery((void*)p, &m, sizeof m) != sizeof m) return false;
        if (m.State != MEM_COMMIT || (m.Protect & PAGE_GUARD) || m.Protect == PAGE_NOACCESS)
            return false;
        return p + len <= (uintptr_t)m.BaseAddress + m.RegionSize;
    };
    uintptr_t ebp = (uintptr_t)__builtin_frame_address(0);
    char loc[MAX_PATH + 32];
    for (int i = 0; i < 48 && ebp; ++i) {
        if (!readable(ebp, 8)) break;
        const uintptr_t ret  = *(uintptr_t*)(ebp + 4);
        const uintptr_t next = *(uintptr_t*)ebp;
        if (ret) {
            describe_addr(ret, loc);
            crash_logf("  [%2d] ret=0x%08X  %s\r\n", i, (unsigned)ret, loc);
        }
        if (next <= ebp) break;
        ebp = next;
    }
    // Raw ESP scan — the EBP chain skips FPO/CRT frames, so the IMMEDIATE
    // abort caller (the th155 native binding / SQVM opcode that aborts) is
    // hidden. Scan the live stack for th155 code return addresses to recover it.
    crash_logf("  --- raw stack scan (th155 ret addrs) ---\r\n");
    volatile uint32_t marker = 0;
    const uint32_t* sp = (const uint32_t*)&marker;
    for (int k = 0, shown = 0; k < 400 && shown < 28; ++k) {
        if (!readable((uintptr_t)&sp[k], 4)) break;
        const uintptr_t v = sp[k];
        const uint32_t rva = (uint32_t)(v - base_address);
        if (rva >= 0x1000 && rva < 0x300000) {
            describe_addr(v, loc);
            crash_logf("    sp[+0x%03X] rva=%08X  %s\r\n", k * 4, rva, loc);
            ++shown;
        }
    }
    crash_logf("==== END FASTFAIL ====\r\n");
}

#define FF_HOOK(idx, nm)                              \
    static void __cdecl ff_hook_##idx() {             \
        log_fastfail_stack(nm, (const uint32_t*)_AddressOfReturnAddress()); \
        TerminateProcess(GetCurrentProcess(), 0xC0000409u); \
    }
FF_HOOK(0, "__report_gsfailure")
FF_HOOK(1, "__report_securityfailure")
FF_HOOK(2, "__scrt_fastfail")
FF_HOOK(3, "__invoke_watson")
FF_HOOK(4, "abort")

// __purecall (th155 0x2F9041) -> abort. A pure-virtual call = a virtual on an
// object whose vtable is (back to) an abstract base's: mid-destruction, or a
// rewound/stale object. abort()'s own frame can't tell us which object; hook
// __purecall itself with a naked shim so ECX (`this` of the virtual call) and
// the exact vtable call site are captured before the abort path runs.
static void* g_purecall_orig = nullptr;
static void __cdecl purecall_log(uint32_t ecx, uint32_t ret) {
    crash_logf("\r\n==== __purecall: this=%08X vtable=%08X region=%s ret=%08X f=%d rb=%d ====\r\n",
               ecx, (ecx >= 0x10000 && !IsBadReadPtr((void*)(uintptr_t)ecx, 4)) ? *(uint32_t*)(uintptr_t)ecx : 0,
               cpp_arena::region_of((const void*)(uintptr_t)ecx),
               ret, gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb);
    char loc[MAX_PATH + 32];
    describe_addr(ret, loc);
    crash_logf("  call site: %s\r\n", loc);
    if (ecx >= 0x10000) {
        const uint32_t* o = (const uint32_t*)(uintptr_t)ecx;
        if (!IsBadReadPtr(o, 32)) {
            crash_logf("  obj[0..7]: %08X %08X %08X %08X %08X %08X %08X %08X\r\n",
                       o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7]);
            uint32_t rva = 0, sz = 0, pay = 0;
            if (cpp_arena::describe_block(ecx, &rva, &sz, &pay))
                crash_logf("  arena block payload=%08X size=%u alloc_rva=%08X\r\n", pay, sz, rva);
        }
    }
}
static naked void purecall_hook_entry() {
    __asm {
        push ecx                    // preserve `this` for the original path
        push dword ptr [esp+4]      // ret (the vtable call site)
        push ecx                    // this
        call purecall_log
        add  esp, 8
        pop  ecx
        jmp  dword ptr [g_purecall_orig]
    }
}
static SafetyHookInline g_h_purecall{}, g_h_terminate{};
static void __cdecl terminate_hook() {
    crash_logf("\r\n==== std::terminate f=%d rb=%d ====\r\n",
               gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb);
    scan_rets((const uint32_t*)_AddressOfReturnAddress(), 40, 12, "    ");
    g_h_terminate.unsafe_ccall<void>();
}

// --- process-exit interception ----------------------------------------------
// If the crash is not an exception at all — a clean ExitProcess / Terminate-
// Process / RaiseFailFastException from some th155 error path — these catch it
// with the call stack intact.
static SafetyHookInline g_h_exit{}, g_h_term{}, g_h_raiseff{};

static void __stdcall hook_exitprocess(UINT code) {
    // A clean exit(0) — the harness's CSS idle-exit and normal shutdown — is
    // not a crash; logging a FASTFAIL stack for it makes a non-empty crash log
    // that reads as a failure in a sweep. Only dump for a non-zero (error) code.
    // Log EVERY exit (code 0 included): a silent process death with no WER
    // event and no crash-log entry can only be ExitProcess(0) — the window's
    // close button, th155's own quit paths, or ours. The stack tells which.
    log_fastfail_stack(code ? "ExitProcess" : "ExitProcess(0) [clean exit / window close]");
    crash_logf("  exit code = 0x%08X\r\n", code);
    // Skip the orderly ExitProcess teardown (DLL detach, CRT statics, thread
    // rundown): the rig left a zombie th155 whose one remaining thread never
    // finished (HasExited=true, thread Running) and which pinned the log file.
    // Nothing in this process needs an orderly shutdown; flush our log and die.
    log_flush();
    TerminateProcess(GetCurrentProcess(), code);
    g_h_exit.unsafe_stdcall<void>(code);   // not reached
}
static BOOL __stdcall hook_terminateprocess(HANDLE h, UINT code) {
    if (h == GetCurrentProcess() || h == (HANDLE)(LONG_PTR)-1) {
        log_fastfail_stack("TerminateProcess");
        crash_logf("  exit code = 0x%08X\r\n", code);
    }
    return g_h_term.unsafe_stdcall<BOOL>(h, code);
}
// HEAP-CORRUPTION ATTRIBUTION: ntdll's RtlpHeapHandleError -> RtlReportCriticalFailure
// -> RtlReportException(record, ctx, flags) -> __fastfail. The fast-fail bypasses every
// hook, but RtlReportException is exported and runs first on the DETECTING thread:
// dump its stack (the RtlFreeHeap/RtlAllocateHeap caller = the block's owner).
static SafetyHookInline g_h_rtlreport{};
static LONG __stdcall hook_rtlreportexception(EXCEPTION_RECORD* rec, void* ctx, DWORD flags) {
    crash_logf("\r\n==== RtlReportException code=0x%08X (heap corruption path) ====\r\n",
               rec ? (unsigned)rec->ExceptionCode : 0u);
    if (rec && rec->NumberParameters) {
        for (DWORD i = 0; i < rec->NumberParameters && i < 4; ++i)
            crash_logf("  param[%u]=0x%08X\r\n", i, (unsigned)rec->ExceptionInformation[i]);
    }
    log_fastfail_stack("RtlReportException");
    log_flush();
    return g_h_rtlreport.unsafe_stdcall<LONG>(rec, ctx, flags);
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

// HANG DIAGNOSIS: dump every thread's stack (toolhelp enumeration; suspend ->
// context -> ebp walk -> resume). Called by the gekko_bridge hang watchdog when
// the forward frame stops advancing, so a silent stall becomes a named loop.
// ON-DEMAND HANG DUMP: a dedicated thread (independent of the watchdog, the sim
// and the game loop) polls for a trigger file next to the exe; when
// "hangdump.now" appears it deletes it and dumps every thread's stack. Lets a
// frozen process be attributed from outside without a debugger.
static DWORD __stdcall ondemand_dump_thread(void*) {
    for (;;) {
        Sleep(500);
        if (GetFileAttributesA("hangdump.now") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("hangdump.now");
            dump_all_thread_stacks("on-demand (hangdump.now)");
            log_flush();
        }
    }
    return 0;
}
void start_ondemand_dump_thread() {
    static bool started = false;
    if (started) return;
    started = true;
    HANDLE h = CreateThread(nullptr, 0, ondemand_dump_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    log_printf("crash_handler: on-demand hangdump thread up (touch hangdump.now)\n");
}

void dump_all_thread_stacks(const char* why) {
    log_printf("[hangdump] === %s ===\n", why);
    auto readable = [](uintptr_t p, size_t len) -> bool {
        MEMORY_BASIC_INFORMATION m;
        if (VirtualQuery((void*)p, &m, sizeof m) != sizeof m) return false;
        if (m.State != MEM_COMMIT || (m.Protect & PAGE_GUARD) || m.Protect == PAGE_NOACCESS)
            return false;
        return p + len <= (uintptr_t)m.BaseAddress + m.RegionSize;
    };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    char loc[MAX_PATH + 32];
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                              te.th32ThreadID);
        if (!h) continue;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(h, &c)) {
                describe_addr(c.Eip, loc);
                log_printf("[hangdump] tid=%u eip=%08X %s\n",
                           te.th32ThreadID, (unsigned)c.Eip, loc);
                uintptr_t ebp = c.Ebp;
                for (int i = 0; i < 24 && ebp; ++i) {
                    if (!readable(ebp, 8)) break;
                    uintptr_t ret  = *(uintptr_t*)(ebp + 4);
                    uintptr_t next = *(uintptr_t*)ebp;
                    if (ret) {
                        describe_addr(ret, loc);
                        log_printf("[hangdump]   [%2d] ret=%08X %s\n", i,
                                   (unsigned)ret, loc);
                    }
                    if (next <= ebp) break;
                    ebp = next;
                }
                // th155 is FPO (no EBP chain) -> the walk above is usually empty.
                // Raw ESP scan: print every stack slot that looks like a return
                // address into th155.exe or Netcode.dll (symbolizable with the
                // PDB). Order = innermost first.
                {
                    uintptr_t sp = c.Esp; int shown = 0;
                    for (int k = 0; k < 512 && shown < 28; ++k) {
                        uintptr_t slot = sp + (uintptr_t)k * 4;
                        if (!readable(slot, 4)) break;
                        uintptr_t v = *(uintptr_t*)slot;
                        if (v < 0x10000) continue;
                        describe_addr(v, loc);
                        if (strstr(loc, "th155.exe+") || strstr(loc, "Netcode.dll+")) {
                            log_printf("[hangdump]   sp[+0x%03X] %s\n", k * 4, loc);
                            ++shown;
                        }
                    }
                }
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    log_printf("[hangdump] === end ===\n");
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
    g_h_purecall = safetyhook::create_inline(base + 0x2f9041, (void*)purecall_hook_entry);
    g_purecall_orig = g_h_purecall ? (void*)g_h_purecall.trampoline().address() : nullptr;
    g_h_terminate = safetyhook::create_inline(base + 0x306738, (void*)terminate_hook);
    log_printf("crash_handler: __purecall hook %s, terminate hook %s\n",
               g_h_purecall.enabled() ? "OK" : "FAIL", g_h_terminate.enabled() ? "OK" : "FAIL");

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        void* pe = (void*)GetProcAddress(k32, "ExitProcess");
        void* pt = (void*)GetProcAddress(k32, "TerminateProcess");
        void* pr = (void*)GetProcAddress(k32, "RaiseFailFastException");
        if (pe) g_h_exit    = safetyhook::create_inline(pe, (void*)hook_exitprocess);
        if (pt) g_h_term    = safetyhook::create_inline(pt, (void*)hook_terminateprocess);
        if (pr) g_h_raiseff = safetyhook::create_inline(pr, (void*)hook_raiseff);
        if (HMODULE nt = GetModuleHandleA("ntdll.dll")) {
            void* rr = (void*)GetProcAddress(nt, "RtlReportException");
            if (rr) g_h_rtlreport = safetyhook::create_inline(rr, (void*)hook_rtlreportexception);
            log_printf("crash_handler: RtlReportException hook %s\n", g_h_rtlreport ? "OK" : "FAIL");
        }
        log_printf("crash_handler: exit hooks exit=%d term=%d raiseff=%d\n",
                   (int)g_h_exit.enabled(), (int)g_h_term.enabled(),
                   (int)g_h_raiseff.enabled());
    }
}

} // namespace crash_handler
