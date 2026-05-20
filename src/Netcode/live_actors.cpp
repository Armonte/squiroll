// safetyhook MUST be included before any of squiroll's headers because
// util.h #define's every calling-convention keyword (cdecl, stdcall,
// thiscall, fastcall, vectorcall) as `__attribute__((...))` macros, and
// safetyhook uses those identifiers as method names on its InlineHook
// class (e.g. `hook.thiscall<int>(...)`). Once safetyhook is parsed the
// macros can come back in via patch_utils.h with no harm done.
#include <safetyhook.hpp>

#include "live_actors.h"
#include "patch_utils.h"
#include "log.h"

// Live ManbowActor2D* registry, maintained via function-entry hooks on
// the five Actor2DManager::CreateActor2D* variants plus Actor2D::Release.
//
// Hooks are installed with safetyhook::InlineHook — handles trampoline
// allocation, length-disassembly via Zydis, RIP-relative fix-ups, thread
// suspension during install/uninstall, the whole deal. Originally tried
// a hand-rolled hotpatch_entry + mini-LDE; that worked but exposed two
// subtle bugs (6-byte JMP overwrite corrupting func+5, calling-convention
// mismatch on thiscall via the wrong number of declared args). safetyhook
// catches that class of bug at template-instantiation time and never
// touches bytes it can't relocate cleanly.

namespace {

// Fixed-size flat array, no heap allocation. STL containers triggered
// allocator interaction with the game's heap mid-CreateActor2D which
// occasionally crashed; a bare array sidesteps that entirely. 2048 slots
// is ~10x the worst-case live actor count we see in a real match.
constexpr size_t LIVE_CAP = 2048;
static ManbowActor2D* g_live[LIVE_CAP];
static size_t         g_live_count = 0;

static inline void g_live_add(ManbowActor2D* a) {
    if (g_live_count < LIVE_CAP) g_live[g_live_count++] = a;
}

static inline void g_live_remove(ManbowActor2D* a) {
    for (size_t i = 0; i < g_live_count; ++i) {
        if (g_live[i] == a) {
            g_live[i] = g_live[--g_live_count]; // swap-with-last + shrink
            return;
        }
    }
}

// One SafetyHookInline per target. Default-constructed = inactive.
// safetyhook::create_inline() returns one of these and tears it down
// when the SafetyHookInline goes out of scope (here = process exit).
static SafetyHookInline g_create_hook{};
static SafetyHookInline g_create_trail_hook{};
static SafetyHookInline g_create_stencil_hook{};
static SafetyHookInline g_create_dyn_hook{};
static SafetyHookInline g_create_3d_hook{};
static SafetyHookInline g_release_hook{};

// Hook bodies. Declared __thiscall so the prologue matches the game's
// MSVC __thiscall ABI. We use safetyhook's `thiscall<RetT>(args...)`
// helper to invoke the original — it routes through a proper thiscall
// function-pointer cast, no manual register juggling.
//
// Manager::CreateActor2D* take 8 args total (this + 7 stack): the
// decompiler initially showed 7 but missed [ebp+0x20]. Get this wrong
// and `ret N` will pop the wrong byte count → garbage return address.

#define MAKE_CREATE_HOOK(NAME, HOOK)                                          \
    int thiscall NAME(void* self,                                           \
                        ManbowActor2D* type, uint32_t* unk,                   \
                        int x, int y, int dir, int unk2, int unk3)            \
    {                                                                         \
        int r = HOOK.unsafe_thiscall<int>(self, type, unk, x, y, dir, unk2, unk3);   \
        if (type) g_live_add(type);                                           \
        return r;                                                             \
    }

MAKE_CREATE_HOOK(hook_create,         g_create_hook)
MAKE_CREATE_HOOK(hook_create_trail,   g_create_trail_hook)
MAKE_CREATE_HOOK(hook_create_stencil, g_create_stencil_hook)
MAKE_CREATE_HOOK(hook_create_dyn,     g_create_dyn_hook)
MAKE_CREATE_HOOK(hook_create_3d,      g_create_3d_hook)

int thiscall hook_release(ManbowActor2D* self) {
    // Drop from set BEFORE the original runs — once Release returns the
    // actor's memory is recycled into the SharedPoolAllocator pool.
    if (self) g_live_remove(self);
    return g_release_hook.unsafe_thiscall<int>(self);
}

} // namespace

// --- Function entry RVAs ----------------------------------------------------
// Verified by reading prologue bytes directly from the on-disk th155.exe
// (file ImageBase 0x400000). If the binary updates, re-verify in IDA
// (fresh IDB at proper ImageBase) and update these.
#define CREATE_ACTOR2D_ADDR         (0x9E340_R)
#define CREATE_ACTOR2D_TRAIL_ADDR   (0x9E470_R)
#define CREATE_ACTOR2D_STENCIL_ADDR (0x9E610_R)
#define CREATE_ACTOR2D_DYNAMIC_ADDR (0x9E7B0_R)
#define CREATE_ACTOR3D_ADDR         (0x9E950_R)
#define ACTOR2D_RELEASE_ADDR        (0xC1230_R)

namespace live_actors {

void install() {
    g_create_hook         = safetyhook::create_inline((void*)CREATE_ACTOR2D_ADDR,         (void*)hook_create);
    g_create_trail_hook   = safetyhook::create_inline((void*)CREATE_ACTOR2D_TRAIL_ADDR,   (void*)hook_create_trail);
    g_create_stencil_hook = safetyhook::create_inline((void*)CREATE_ACTOR2D_STENCIL_ADDR, (void*)hook_create_stencil);
    g_create_dyn_hook     = safetyhook::create_inline((void*)CREATE_ACTOR2D_DYNAMIC_ADDR, (void*)hook_create_dyn);
    g_create_3d_hook      = safetyhook::create_inline((void*)CREATE_ACTOR3D_ADDR,         (void*)hook_create_3d);
    g_release_hook        = safetyhook::create_inline((void*)ACTOR2D_RELEASE_ADDR,        (void*)hook_release);

    int ok = g_create_hook.enabled()
           + g_create_trail_hook.enabled()
           + g_create_stencil_hook.enabled()
           + g_create_dyn_hook.enabled()
           + g_create_3d_hook.enabled()
           + g_release_hook.enabled();
    log_printf("live_actors::install: %d/6 safetyhook InlineHooks active\n", ok);
}

size_t snapshot(ManbowActor2D** out_buf, size_t max_count) {
    size_t n = g_live_count < max_count ? g_live_count : max_count;
    memcpy(out_buf, g_live, n * sizeof(ManbowActor2D*));
    return n;
}

size_t count() { return g_live_count; }

} // namespace live_actors
