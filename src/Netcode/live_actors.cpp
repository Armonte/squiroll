#if 0   // disabled until gekko_bridge is wired into the build

#include "live_actors.h"
#include "patch_utils.h"
#include "log.h"

#include <unordered_set>

namespace {

std::unordered_set<ManbowActor2D*> g_live;

// All five CreateActor2D* wrappers on Manbow::Actor2DManager share the
// same return-the-actor pattern: the caller passes a partially-constructed
// Actor2D in `type`, the manager fills it in and returns it. So our hook
// can be identical for all five.
//
// Originals (RVAs in canonical scheme; squiroll's _R macro adds image base):
//   Manbow::Actor2DManager::CreateActor2D        0xFE340_R
//   Manbow::Actor2DManager::CreateActor2DTrail   0xFE470_R
//   Manbow::Actor2DManager::CreateActor2DStencil 0xFE610_R
//   Manbow::Actor2DManager::CreateActor2DDynamic 0xFE7B0_R
//   Manbow::Actor2DManager::CreateActor3D        0xFE950_R

typedef int thiscall create_actor_t(
    void* self, ManbowActor2D* type, _DWORD* unk,
    int x, int y, int dir, int unk2);

static create_actor_t* g_orig_create        = nullptr;
static create_actor_t* g_orig_create_trail  = nullptr;
static create_actor_t* g_orig_create_stencil= nullptr;
static create_actor_t* g_orig_create_dyn    = nullptr;
static create_actor_t* g_orig_create_3d     = nullptr;

#define MAKE_HOOK(NAME, ORIG)                                            \
    int thiscall NAME(void* self, ManbowActor2D* type, _DWORD* unk,      \
                      int x, int y, int dir, int unk2)                   \
    {                                                                    \
        int r = ORIG(self, type, unk, x, y, dir, unk2);                  \
        g_live.insert(type);                                             \
        return r;                                                        \
    }

MAKE_HOOK(hook_create,         g_orig_create)
MAKE_HOOK(hook_create_trail,   g_orig_create_trail)
MAKE_HOOK(hook_create_stencil, g_orig_create_stencil)
MAKE_HOOK(hook_create_dyn,     g_orig_create_dyn)
MAKE_HOOK(hook_create_3d,      g_orig_create_3d)

// Actor2D::Release wrapper at 0x121230_R. Pre-Release we drop from the
// set; the original then does the actual destructor work.
typedef int thiscall release_t(ManbowActor2D* this_);
static release_t* g_orig_release = nullptr;

int thiscall hook_release(ManbowActor2D* this_) {
    g_live.erase(this_);
    return g_orig_release(this_);
}

} // namespace

namespace live_actors {

void install() {
    // TODO: hot-patching arbitrary thiscall functions via hotpatch_call
    // requires the *call site*, not the function entry. The squiroll-style
    // approach is to find every caller of Actor2DManager::CreateActor2D
    // (and its siblings) and redirect each call site to our hook.
    //
    // Easier short-term: write a 5-byte jump at the function entry pointing
    // to our hook, save the original prologue, and reconstruct the orig
    // pointer via a trampoline. squiroll has hotpatch_jump for this; pair
    // with a small allocated trampoline that runs the original prologue
    // bytes then jumps to entry+5. See patch_utils.{h,cpp}.
    //
    // For now this is a no-op so the build links cleanly. Wire it up when
    // adding gekko_bridge for real.
    log_printf("live_actors::install: stub — hooks not yet installed\n");
}

size_t snapshot(ManbowActor2D** out_buf, size_t max_count) {
    size_t i = 0;
    for (auto* a : g_live) {
        if (i >= max_count) break;
        out_buf[i++] = a;
    }
    return i;
}

size_t count() { return g_live.size(); }

} // namespace live_actors

#endif // 0
