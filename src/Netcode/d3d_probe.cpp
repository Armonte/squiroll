// [#28] D3D11 resource probe — see d3d_probe.h.
//
// The driver crash (nvwgf2um exec-at-heap) is a reference-consistency bug: the
// UMD holds a persistent pointer INTO tf4-mspace region A, and rollback rewrites
// that memory. GPU-drain and STEP-0-skip were both disproven as fixes. To pin
// (or exclude) the right memory we first have to know WHICH D3D resource carries
// a region-A pointer. This range-checks every resource-creating / mapping /
// updating call against [regionA.base, regionA.base+size) and logs the hits.
//
// COM vtable patching (not inline hooks): th155 talks to D3D through the device
// and immediate-context vtables at 0x4DAE9C / 0x4DAE98. Patching the shared
// vtable slot catches every call through that interface.

#include <cstdint>
#include <cstdio>
#include <windows.h>
#include <d3d11.h>

#include "log.h"
#include "util.h"          // _R base-relocation for the device/context globals
#include "tf4_arena.h"     // region-A base/size

// [#28] re-sim flag: non-zero while a rollback re-simulates a frame. Any D3D
// call issued during a re-sim means the "headless" re-sim is actually driving
// the D3D pipeline again -> extra/duplicate driver work against inconsistent
// state = a universal (NVIDIA+AMD) crash cause and a clean fix target.
namespace gekko_bridge { extern int g_trace_rb; }

#define d3d11_dev ((ID3D11Device**)0x4DAE9C_R)
#define d3d11_imm ((ID3D11DeviceContext**)0x4DAE98_R)

// ---- region-A range (logged once armed, for reference) --------------------
static uint8_t* g_ra_base = nullptr;
static uint32_t g_ra_size = 0;

// Which snapshot arena backs pointer p (nullptr = none). Bases are fixed
// (ASLR-off, tf4_arena/sq_arena/... install logs): sq 0x24M(64), bullet
// 0x2AM(32), cpp 0x30M(128), tf4A 0x3AM(128), tf4B 0x44M(32). Any D3D resource
// SOURCED from one of these lives in memory the rollback rewrites -> if the
// driver keeps reading it, that region must be excluded from the restore.
static const char* which_arena(const void* p) {
    uintptr_t a = (uintptr_t)p;
    if (a >= 0x24000000u && a < 0x28000000u) return "sq";
    if (a >= 0x2A000000u && a < 0x2C000000u) return "bullet";
    if (a >= 0x30000000u && a < 0x38000000u) return "cpp";
    if (a >= 0x3A000000u && a < 0x42010000u) return "tf4A";
    if (a >= 0x44000000u && a < 0x46000000u) return "tf4B";
    return nullptr;
}

// Throttle: cap total hit lines so a firehose can't drown the log. A resource
// created once is what we care about; per-frame Map spam is capped separately.
static int g_budget = 400;
static inline bool spend() { if (g_budget <= 0) return false; --g_budget; return true; }

// Bind-validation budget (separate): only SUSPICIOUS bound pointers are logged
// (self-filtering), so this catches the stale binding the rollback planted.
static int g_bind_budget = 200;

// Is p a pointer into ANY snapshot arena [0x24000000, 0x46000000)? Real D3D
// resources live at 0x05M (runtime) / 0x16M (objects); a bound "resource"
// pointer INSIDE an arena is a rolled-back sim value mis-bound as a resource.
static inline bool in_any_arena(const void* p) {
    uintptr_t a = (uintptr_t)p;
    return a >= 0x24000000u && a < 0x46000000u;
}

// Lightweight COM-object sanity: object memory committed+readable AND its
// vtable slot committed+readable. A stale/dangling binding fails this without
// faulting (VirtualQuery never dereferences).
// One-shot "this hook fired on thread X" — confirms the context vtable patch
// is actually on th155's render path (else the bind/param checks are inert).
static void note_tid(const char* tag, bool& once) {
    if (once) return;
    once = true;
    log_printf("[#28tid] %s fired tid=%lu\n", tag, GetCurrentThreadId());
}

static bool bad_com(void* p) {
    if (!p) return false;                       // NULL = legit unbind
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return true;
    const DWORD rok = PAGE_READONLY|PAGE_READWRITE|PAGE_EXECUTE_READ|
                      PAGE_EXECUTE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & rok)) return true;
    void* vt = *(void**)p;                       // COM vtable ptr
    if (!VirtualQuery(vt, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return true;
    if (!(mbi.Protect & rok)) return true;
    return false;
}

// Flag a bound resource pointer that is either inside an arena (a rolled-back
// value) or fails the COM sanity check. Returns true if it logged.
static bool check_bound(const char* what, int slot, void* p) {
    if (!p) return false;
    if (!in_any_arena(p) && !bad_com(p)) return false;
    if (g_bind_budget <= 0) return false;
    --g_bind_budget;
    log_printf("[d3dprobe] **BAD BIND** %s[%d]=%p arena=%d badcom=%d\n",
               what, slot, p, in_any_arena(p) ? 1 : 0, bad_com(p) ? 1 : 0);
    return true;
}

static void* patch_vt(void** vt, int idx, void* hook);   // fwd (defined below)
static ULONG STDMETHODCALLTYPE h_TexRelease(IUnknown* self);   // fwd

// ---- originals ------------------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE* CreateBuffer_t)(
    ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*,
    ID3D11Buffer**);
typedef HRESULT (STDMETHODCALLTYPE* CreateTexture2D_t)(
    ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*,
    ID3D11Texture2D**);
typedef HRESULT (STDMETHODCALLTYPE* Map_t)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
    D3D11_MAPPED_SUBRESOURCE*);
typedef void (STDMETHODCALLTYPE* UpdateSubresource_t)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*,
    UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* CreateSRV_t)(
    ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*,
    ID3D11ShaderResourceView**);
typedef ULONG (STDMETHODCALLTYPE* Release_t)(IUnknown*);
typedef void (STDMETHODCALLTYPE* PSSetSRV_t)(
    ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
typedef void (STDMETHODCALLTYPE* IASetVB_t)(
    ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*,
    const UINT*);
typedef void (STDMETHODCALLTYPE* DrawIndexed_t)(
    ID3D11DeviceContext*, UINT, UINT, INT);
typedef void (STDMETHODCALLTYPE* Draw_t)(
    ID3D11DeviceContext*, UINT, UINT);

static CreateBuffer_t      o_CreateBuffer      = nullptr;
static CreateTexture2D_t   o_CreateTexture2D   = nullptr;
static Map_t               o_Map               = nullptr;
static UpdateSubresource_t o_UpdateSubresource = nullptr;
static CreateSRV_t         o_CreateSRV         = nullptr;
static Release_t           o_TexRelease        = nullptr;
static PSSetSRV_t          o_PSSetSRV          = nullptr;
static IASetVB_t           o_IASetVB           = nullptr;
static DrawIndexed_t       o_DrawIndexed       = nullptr;
static Draw_t              o_Draw              = nullptr;

// Texture destruction + SRV creation during battle = the resource churn that
// dangles the driver's async references. Separate budgets so texture-DESTROY
// and SRV-CREATE lines survive even after the 400 create-hits exhaust g_budget.
static int  g_rel_budget = 300;
static int  g_srv_budget = 1200;
static bool g_tex_vt_patched = false;

// ---- hooks ----------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE h_CreateBuffer(
    ID3D11Device* self, const D3D11_BUFFER_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out) {
    HRESULT hr = o_CreateBuffer(self, desc, init, out);
    const char* ar = (desc && init) ? which_arena(init->pSysMem) : nullptr;
    if (ar && spend()) {
        log_printf("[d3dprobe] CreateBuffer bytes=%u usage=%d bind=0x%x "
                   "cpuacc=0x%x misc=0x%x sysmem=%p obj=%p **ARENA=%s**\n",
                   desc->ByteWidth, (int)desc->Usage, desc->BindFlags,
                   desc->CPUAccessFlags, desc->MiscFlags, init->pSysMem,
                   out ? (void*)*out : nullptr, ar);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE h_CreateTexture2D(
    ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    HRESULT hr = o_CreateTexture2D(self, desc, init, out);
    const char* ar = (desc && init) ? which_arena(init->pSysMem) : nullptr;
    // Only log NON-tf4A texture uploads: tf4A immutable texture spam is already
    // characterized (400 hits, copied at create). We hunt DYNAMIC/DEFAULT or
    // OTHER-arena resources the driver keeps reading.
    if (ar && desc->Usage != D3D11_USAGE_IMMUTABLE && spend()) {
        log_printf("[d3dprobe] CreateTexture2D %ux%u fmt=%d usage=%d bind=0x%x "
                   "sysmem=%p obj=%p **ARENA=%s**\n",
                   desc->Width, desc->Height, (int)desc->Format,
                   (int)desc->Usage, desc->BindFlags, init->pSysMem,
                   out ? (void*)*out : nullptr, ar);
    }
    // Patch the ID3D11Texture2D vtable Release (index 2) ONCE, from the first
    // texture we see, so we can log texture DESTRUCTION during battle. All
    // textures from this driver share the vtable, so one patch covers them all.
    if (!g_tex_vt_patched && out && *out) {
        g_tex_vt_patched = true;
        void** tvt = *(void***)*out;
        o_TexRelease = (Release_t)patch_vt(tvt, 2, (void*)h_TexRelease);
        log_printf("[d3dprobe] tex vtable Release hooked (obj=%p)\n", (void*)*out);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE h_Map(
    ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub, D3D11_MAP type,
    UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) {
    static bool o = false; note_tid("Map", o);
    { static int b = 40; if (gekko_bridge::g_trace_rb && b > 0) { --b;
        log_printf("[d3dprobe] **MAP DURING RE-SIM** rb=%d res=%p type=%d\n",
                   gekko_bridge::g_trace_rb, (void*)res, (int)type); } }
    HRESULT hr = o_Map(self, res, sub, type, flags, mapped);
    // A Map that HANDS BACK an arena pointer means the driver's staging for this
    // resource lives in an arena -> exactly the persistent reference we hunt
    // (the driver reads it while the app fills it, and every frame after).
    const char* ar = (SUCCEEDED(hr) && mapped) ? which_arena(mapped->pData) : nullptr;
    if (ar && spend()) {
        log_printf("[d3dprobe] Map resource=%p pData=%p maptype=%d "
                   "**ARENA=%s pData**\n", (void*)res, mapped->pData, (int)type, ar);
    }
    return hr;
}

static void STDMETHODCALLTYPE h_UpdateSubresource(
    ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub,
    const D3D11_BOX* box, const void* src, UINT rowpitch, UINT depthpitch) {
    const char* ar = which_arena(src);
    if (ar && spend()) {
        log_printf("[d3dprobe] UpdateSubresource resource=%p src=%p "
                   "rowpitch=%u **ARENA=%s src**\n", (void*)res, src, rowpitch, ar);
    }
    o_UpdateSubresource(self, res, sub, box, src, rowpitch, depthpitch);
}

// A texture whose refcount reaches 0 during BATTLE (log line > first [adv]) is
// the churn we hunt: its 0x16M object frees, but the driver's async command
// queue / a rolled-back sim cache may still reference it -> exec-at-heap.
static ULONG STDMETHODCALLTYPE h_TexRelease(IUnknown* self) {
    ULONG rc = o_TexRelease(self);
    if (rc == 0 && g_rel_budget > 0) {
        --g_rel_budget;
        log_printf("[d3dprobe] TEX_DESTROYED obj=%p\n", (void*)self);
    }
    return rc;
}

static HRESULT STDMETHODCALLTYPE h_CreateSRV(
    ID3D11Device* self, ID3D11Resource* res,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** out) {
    HRESULT hr = o_CreateSRV(self, res, desc, out);
    if (g_srv_budget > 0) {
        --g_srv_budget;
        log_printf("[d3dprobe] CreateSRV res=%p srv=%p\n",
                   (void*)res, out ? (void*)*out : nullptr);
    }
    return hr;
}

// Bind-validation: the render path builds these from rolled-back structures. A
// bound SRV / vertex-buffer pointer that is arena-resident or fails COM sanity
// IS the stale binding the rollback planted -> the crash's proximate cause.
static void STDMETHODCALLTYPE h_PSSetSRV(
    ID3D11DeviceContext* self, UINT start, UINT num,
    ID3D11ShaderResourceView* const* views) {
    static bool o = false; note_tid("PSSetSRV", o);
    if (views) for (UINT i = 0; i < num && i < 16; ++i)
        check_bound("SRV", (int)(start + i), (void*)views[i]);
    o_PSSetSRV(self, start, num, views);
}

static void STDMETHODCALLTYPE h_IASetVB(
    ID3D11DeviceContext* self, UINT start, UINT num,
    ID3D11Buffer* const* bufs, const UINT* strides, const UINT* offsets) {
    static bool o = false; note_tid("IASetVB", o);
    if (bufs) for (UINT i = 0; i < num && i < 16; ++i)
        check_bound("VB", (int)(start + i), (void*)bufs[i]);
    // Numeric params derived from rolled-back actor state: a 0 or absurd stride
    // is the divide-by-zero / OOB-read the NO_STEP0 run exposed (0xC0000094).
    if (strides) for (UINT i = 0; i < num && i < 16; ++i) {
        if ((strides[i] == 0 || strides[i] > 0x4000) && g_bind_budget > 0) {
            --g_bind_budget;
            log_printf("[d3dprobe] **BAD VB STRIDE**[%d]=%u off=%u\n",
                       (int)(start + i), strides[i], offsets ? offsets[i] : 0);
        }
    }
    o_IASetVB(self, start, num, bufs, strides, offsets);
}

static void STDMETHODCALLTYPE h_DrawIndexed(
    ID3D11DeviceContext* self, UINT idx_count, UINT start_idx, INT base_vtx) {
    // An absurd index count / start comes from a rolled-back render structure
    // and drives the driver to read far past the index/vertex buffers.
    if ((idx_count > 0x40000 || start_idx > 0x100000) && g_bind_budget > 0) {
        --g_bind_budget;
        log_printf("[d3dprobe] **BAD DRAW** idx_count=%u start=%u base=%d\n",
                   idx_count, start_idx, base_vtx);
    }
    static bool o = false; note_tid("DrawIndexed", o);
    o_DrawIndexed(self, idx_count, start_idx, base_vtx);
}

static int g_resim_draw_budget = 60;
static void STDMETHODCALLTYPE h_Draw(
    ID3D11DeviceContext* self, UINT vtx_count, UINT start_vtx) {
    static bool o = false; note_tid("Draw", o);
    if (gekko_bridge::g_trace_rb && g_resim_draw_budget > 0) {
        --g_resim_draw_budget;
        log_printf("[d3dprobe] **DRAW DURING RE-SIM** rb=%d vtx=%u\n",
                   gekko_bridge::g_trace_rb, vtx_count);
    }
    o_Draw(self, vtx_count, start_vtx);
}

// Patch one vtable slot; returns the original.
static void* patch_vt(void** vt, int idx, void* hook) {
    DWORD old = 0;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old);
    void* orig = vt[idx];
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vt[idx], sizeof(void*));
    return orig;
}

void d3d_probe_arm() {
    static bool tried = false;
    if (tried) return;
    static const bool on = []{ const char* e = getenv("SQUIROLL_D3D_PROBE");
                               return e && atoi(e); }();
    if (!on) { tried = true; return; }

    ID3D11Device*        dev = d3d11_dev ? *d3d11_dev : nullptr;
    ID3D11DeviceContext* imm = d3d11_imm ? *d3d11_imm : nullptr;
    if (!dev || !imm) return;                 // device not up yet — retry next frame
    if (!tf4_arena::ready()) return;          // arena not intercepted yet — retry
    tried = true;

    g_ra_base = tf4_arena::base(0);
    g_ra_size = tf4_arena::size(0);

    void** dev_vt = *(void***)dev;            // ID3D11Device vtable
    void** imm_vt = *(void***)imm;            // ID3D11DeviceContext vtable

    // Standard D3D11 vtable indices (IUnknown 0..2, ID3D11DeviceChild 3..6):
    //   Device:  CreateBuffer = 3, CreateTexture2D = 5
    //   Context: Map = 14, UpdateSubresource = 48
    // Device: CreateBuffer=3, CreateTexture2D=5, CreateShaderResourceView=7
    // Context: PSSetShaderResources=8, DrawIndexed=12, Map=14,
    //          IASetVertexBuffers=18, UpdateSubresource=48
    o_CreateBuffer      = (CreateBuffer_t)     patch_vt(dev_vt, 3,  (void*)h_CreateBuffer);
    o_CreateTexture2D   = (CreateTexture2D_t)  patch_vt(dev_vt, 5,  (void*)h_CreateTexture2D);
    o_CreateSRV         = (CreateSRV_t)        patch_vt(dev_vt, 7,  (void*)h_CreateSRV);
    o_PSSetSRV          = (PSSetSRV_t)         patch_vt(imm_vt, 8,  (void*)h_PSSetSRV);
    o_DrawIndexed       = (DrawIndexed_t)      patch_vt(imm_vt, 12, (void*)h_DrawIndexed);
    o_Draw              = (Draw_t)             patch_vt(imm_vt, 13, (void*)h_Draw);
    o_Map               = (Map_t)              patch_vt(imm_vt, 14, (void*)h_Map);
    o_IASetVB           = (IASetVB_t)          patch_vt(imm_vt, 18, (void*)h_IASetVB);
    o_UpdateSubresource = (UpdateSubresource_t)patch_vt(imm_vt, 48, (void*)h_UpdateSubresource);

    log_printf("[d3dprobe] ARMED regionA=[%p,+0x%x) dev=%p imm=%p\n",
               g_ra_base, g_ra_size, (void*)dev, (void*)imm);
}
