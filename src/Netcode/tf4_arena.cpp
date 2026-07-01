#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <intrin.h>        // _ReturnAddress

#include "tf4_arena.h"
#include "patch_utils.h"   // _R address literal (via util.h: base_address)
#include "util.h"          // base_address
#include "log.h"           // log_printf (async logger — never printf/fprintf)

// NB: no safetyhook here — this is a pure IAT patch (same VirtualProtect+write
// pattern as the SQUIROLL_DET IAT patches at the end of cpp_arena.cpp install()),
// so the "safetyhook before util.h" include-ordering constraint does not apply.

namespace tf4_arena {
namespace {

// --- th155 addresses (RVA; rebased at use via _R / base_address) ------------
// VirtualAlloc IAT import slot (tf4_mspace_create's `call ds:VirtualAlloc`).
#define IAT_VIRTUALALLOC   (0x388094_R)
// The two mspace pointers tf4_mspace_create stores (dword_4D9F0C[1346]/[1376]).
// Nonzero at early_install() time => the pools already exist => we're too late.
#define MSPACE_A_PTR       (0x4DB414_R)
#define MSPACE_B_PTR       (0x4DB48C_R)
// tf4_mspace_create's two VirtualAlloc call sites live in [0x33200, 0x33300)
// (RVA 0x33234 primary, 0x332B6 secondary; return addresses land in-range).
static constexpr uint32_t CALLER_RVA_LO = 0x33200;
static constexpr uint32_t CALLER_RVA_HI = 0x33300;

// Fixed bases for run-to-run pointer determinism (gravy — falls back to an
// OS-chosen base on failure; write-watch is what matters for correctness).
// Chosen clear of sq(0x24000000/64MB) bullet(0x2A000000/32MB) cpp(0x30000000/128MB).
static constexpr uintptr_t WANT_BASE_A = 0x3A000000;   // 128MB -> [0x3A000000,0x42010000)
static constexpr uintptr_t WANT_BASE_B = 0x44000000;   //  32MB -> [0x44000000,0x46010000)

struct Pool {
    uint8_t* base        = nullptr;
    uint32_t size        = 0;
    bool     intercepted = false;
};
static Pool g_pool[2];              // [0] = region A (primary), [1] = region B (secondary)

static bool g_disabled  = false;    // SQUIROLL_NO_TF4SNAP
static bool g_too_late  = false;    // pools already existed when we installed
static bool g_installed = false;    // early_install() ran

typedef LPVOID(WINAPI* virtualalloc_t)(LPVOID, SIZE_T, DWORD, DWORD);
static virtualalloc_t g_real_valloc = nullptr;

// Replacement VirtualAlloc. Intercepts ONLY tf4_mspace_create's two big pool
// allocations (matched by lpAddress==NULL + size + caller RVA); everything else
// passes straight through to the real VirtualAlloc captured before we patched.
static LPVOID WINAPI hook_virtualalloc(LPVOID lpAddress, SIZE_T dwSize,
                                       DWORD flAllocationType, DWORD flProtect) {
    const uint32_t caller_rva =
        (uint32_t)((uintptr_t)_ReturnAddress() - (uintptr_t)base_address);

    if (lpAddress == nullptr && dwSize >= 0x2000000u &&
        caller_rva >= CALLER_RVA_LO && caller_rva < CALLER_RVA_HI) {
        // Distinguish the pools by size (>=64MB => the ~128MB primary), NOT by
        // call order — robust if the engine ever reorders them.
        const bool is_a = (dwSize >= 0x4000000u);
        void* const  want  = (void*)(is_a ? WANT_BASE_A : WANT_BASE_B);
        const DWORD  flags = flAllocationType | MEM_WRITE_WATCH;

        LPVOID p = g_real_valloc(want, dwSize, flags, flProtect);
        if (!p)   // fixed base unavailable — write-watch is the load-bearing part
            p = g_real_valloc(nullptr, dwSize, flags, flProtect);

        if (p) {
            Pool& pool = g_pool[is_a ? 0 : 1];
            pool.base        = (uint8_t*)p;
            pool.size        = (uint32_t)dwSize;
            pool.intercepted = true;
            log_printf("[tf4_arena] pool %c intercepted base=%p size=%u MB ww=1 "
                       "(fixed=%s caller_rva=%05X)\n",
                       is_a ? 'A' : 'B', p, (uint32_t)(dwSize / (1024u * 1024u)),
                       (p == want) ? "yes" : "no", caller_rva);
        } else {
            log_printf("[tf4_arena] !! pool %c VirtualAlloc FAILED (size=%u) — "
                       "snapshot will skip this region\n",
                       is_a ? 'A' : 'B', (uint32_t)dwSize);
        }
        return p;
    }

    return g_real_valloc(lpAddress, dwSize, flAllocationType, flProtect);
}

} // namespace

void early_install() {
    if (g_installed) return;
    g_installed = true;

    if (getenv("SQUIROLL_NO_TF4SNAP")) {
        g_disabled = true;
        log_printf("[tf4_arena] SQUIROLL_NO_TF4SNAP set — tf4 pool snapshot DISABLED\n");
        return;
    }

    // If the pools already exist we hooked too late to intercept their
    // VirtualAlloc. Learn this from the log rather than silently degrading.
    const uint32_t a_ptr = *(volatile uint32_t*)(MSPACE_A_PTR);
    const uint32_t b_ptr = *(volatile uint32_t*)(MSPACE_B_PTR);
    if (a_ptr || b_ptr) {
        g_too_late = true;
        log_printf("[tf4_arena] TOO LATE — pools already created (A=%08X B=%08X); "
                   "snapshot registration skipped\n", a_ptr, b_ptr);
        return;
    }

    void** iat = (void**)(IAT_VIRTUALALLOC);
    g_real_valloc = (virtualalloc_t)*iat;   // capture the real VirtualAlloc first
    DWORD old = 0;
    if (VirtualProtect(iat, sizeof(void*), PAGE_READWRITE, &old)) {
        *iat = (void*)&hook_virtualalloc;
        VirtualProtect(iat, sizeof(void*), old, &old);
        log_printf("[tf4_arena] VirtualAlloc IAT patched (slot=%p real=%p) — "
                   "awaiting tf4_mspace_create\n", (void*)iat, (void*)g_real_valloc);
    } else {
        g_real_valloc = nullptr;
        log_printf("[tf4_arena] !! VirtualProtect failed on IAT slot %p — "
                   "tf4 pool snapshot DISABLED\n", (void*)iat);
    }
}

bool ready() {
    return !g_disabled && !g_too_late &&
           g_pool[0].intercepted && g_pool[1].intercepted;
}

uint8_t* base(int i) { return (i == 0 || i == 1) ? g_pool[i].base : nullptr; }
uint32_t size(int i) { return (i == 0 || i == 1) ? g_pool[i].size : 0; }

} // namespace tf4_arena
