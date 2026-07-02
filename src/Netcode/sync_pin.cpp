// safetyhook MUST be included before any squiroll header (util.h #defines
// calling-convention keywords safetyhook uses as identifiers).
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "sync_pin.h"
#include "patch_utils.h"   // _R address literal
#include "util.h"
#include "log.h"

// CRT std::mutex internals (static CRT, fixed RVAs, verified in IDA):
//   __Mtx_init_in_situ(_Mtx_t, int)  0x2C3E70 — every std::mutex ctor lands here
//   __Mtx_destroy_in_situ(_Mtx_t)    0x2C3E20
// _Mtx_internal_imp_t on x86 MSVC14 is 0x30 bytes (type, stl_critical_section
// vtable+SRWLOCK, thread_id, count) — pin the whole struct.
#define MTX_INIT_IN_SITU    (0x2C3E70_R)
#define MTX_DESTROY_IN_SITU (0x2C3E20_R)
#define MTX_IMP_SIZE        0x30u
// Raw Win32 CRITICAL_SECTIONs (engine code: ScriptAPI signal locks, ...):
// IAT slots verified in IDA. CRITICAL_SECTION is 24 bytes on x86.
#define IAT_INIT_CS         (0x38808C_R)
#define IAT_INIT_CS_SPIN    (0x3880D4_R)
#define IAT_DELETE_CS       (0x388090_R)
#define CS_SIZE             24u

namespace sync_pin {
namespace {

static constexpr int MAXENT = 16384;
static Ent              g_ent[MAXENT];
static int              g_n         = 0;
static bool             g_installed = false;
static CRITICAL_SECTION g_lock;                 // guards the registry
static uint32_t         g_warn      = 4;

static SafetyHookInline g_h_mtx_init{};
static SafetyHookInline g_h_mtx_destroy{};
typedef void (WINAPI* initcs_t)(LPCRITICAL_SECTION);
typedef BOOL (WINAPI* initcs_spin_t)(LPCRITICAL_SECTION, DWORD);
typedef void (WINAPI* delcs_t)(LPCRITICAL_SECTION);
static initcs_t      g_real_initcs      = nullptr;
static initcs_spin_t g_real_initcs_spin = nullptr;
static delcs_t       g_real_delcs       = nullptr;

static void reg(uint32_t addr, uint32_t len) {
    if (!addr) return;
    EnterCriticalSection(&g_lock);
    // Dedupe: a freed+reused slot re-initialized at the same address just
    // refreshes the existing entry (len may differ CS vs mutex — keep larger).
    for (int i = 0; i < g_n; ++i) {
        if (g_ent[i].addr == addr) {
            if (len > g_ent[i].len) g_ent[i].len = len;
            LeaveCriticalSection(&g_lock);
            return;
        }
    }
    if (g_n < MAXENT) {
        g_ent[g_n].addr = addr;
        g_ent[g_n].len  = len;
        ++g_n;
    } else if (g_warn) {
        --g_warn;
        log_printf("[sync_pin] !! registry FULL (%d) — raise MAXENT\n", MAXENT);
    }
    LeaveCriticalSection(&g_lock);
}

static void unreg(uint32_t addr) {
    if (!addr) return;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_n; ++i) {
        if (g_ent[i].addr == addr) {
            g_ent[i] = g_ent[--g_n];
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
}

// --- hooks ---------------------------------------------------------------

static void cdecl mtx_init_hook(void* mtx, int type) {
    reg((uint32_t)(uintptr_t)mtx, MTX_IMP_SIZE);
    g_h_mtx_init.unsafe_ccall<void>(mtx, type);
}
static void cdecl mtx_destroy_hook(void* mtx) {
    unreg((uint32_t)(uintptr_t)mtx);
    g_h_mtx_destroy.unsafe_ccall<void>(mtx);
}
static void WINAPI initcs_hook(LPCRITICAL_SECTION cs) {
    reg((uint32_t)(uintptr_t)cs, CS_SIZE);
    g_real_initcs(cs);
}
static BOOL WINAPI initcs_spin_hook(LPCRITICAL_SECTION cs, DWORD spin) {
    reg((uint32_t)(uintptr_t)cs, CS_SIZE);
    return g_real_initcs_spin(cs, spin);
}
static void WINAPI delcs_hook(LPCRITICAL_SECTION cs) {
    unreg((uint32_t)(uintptr_t)cs);
    g_real_delcs(cs);
}

static void* patch_iat(uintptr_t slot, void* fn) {
    void** p = (void**)slot;
    void* old = *p;
    DWORD prot = 0;
    if (VirtualProtect(p, sizeof(void*), PAGE_READWRITE, &prot)) {
        *p = fn;
        VirtualProtect(p, sizeof(void*), prot, &prot);
        return old;
    }
    return nullptr;
}

} // namespace

void install() {
    if (g_installed) return;
    g_installed = true;
    InitializeCriticalSection(&g_lock);

    g_h_mtx_init    = safetyhook::create_inline((void*)MTX_INIT_IN_SITU,
                                                (void*)mtx_init_hook);
    g_h_mtx_destroy = safetyhook::create_inline((void*)MTX_DESTROY_IN_SITU,
                                                (void*)mtx_destroy_hook);
    g_real_initcs      = (initcs_t)     patch_iat(IAT_INIT_CS,      (void*)initcs_hook);
    g_real_initcs_spin = (initcs_spin_t)patch_iat(IAT_INIT_CS_SPIN, (void*)initcs_spin_hook);
    g_real_delcs       = (delcs_t)      patch_iat(IAT_DELETE_CS,    (void*)delcs_hook);

    log_printf("[sync_pin] install: mtx=%d/%d iat cs=%d spin=%d del=%d\n",
               (int)g_h_mtx_init.enabled(), (int)g_h_mtx_destroy.enabled(),
               g_real_initcs != nullptr, g_real_initcs_spin != nullptr,
               g_real_delcs != nullptr);
}

void pin(uint32_t addr, uint32_t len) {
    if (!g_installed) return;
    reg(addr, len);
}

void forget_range(uint32_t lo, uint32_t hi) {
    if (!g_installed || !g_n) return;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_n; ) {
        if (g_ent[i].addr >= lo && g_ent[i].addr < hi)
            g_ent[i] = g_ent[--g_n];
        else
            ++i;
    }
    LeaveCriticalSection(&g_lock);
}

int snapshot_live(uint8_t* buf, uint32_t cap, Ent* out, int maxn) {
    if (!g_installed) return 0;
    EnterCriticalSection(&g_lock);
    int      n   = 0;
    uint32_t pos = 0;
    for (int i = 0; i < g_n && n < maxn; ++i) {
        if (pos + g_ent[i].len > cap) break;
        memcpy(buf + pos, (const void*)(uintptr_t)g_ent[i].addr, g_ent[i].len);
        out[n]      = g_ent[i];
        pos        += g_ent[i].len;
        ++n;
    }
    LeaveCriticalSection(&g_lock);
    return n;
}

} // namespace sync_pin
