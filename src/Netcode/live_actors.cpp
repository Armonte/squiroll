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
// Manbow::Actor2DManager::AllocateActor (registration) and
// Manbow::Actor2D::Release (unregistration).
//
// AllocateActor is the single chokepoint every actor birth funnels
// through: all five CreateActor2D* variants (CreateActor2D / *Trail /
// *Stencil / *Dynamic / CreateActor3D) call it and it RETURNS the real
// ManbowActor2D*. The earlier code hooked the five CreateActor2D*
// entries and registered their 2nd argument — but that arg is an
// OUT-param: a caller stack buffer where a Sqrat::Object result is
// constructed, NOT the actor. So g_live filled with stack-address
// garbage. Verified in IDA: AllocateActor @ 0x9E250 returns the actor;
// CreateActor2D's a2 is `*(a2)=&Sqrat::Object::vftable; ... return a2`.
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

// Deferred-release queue. While g_defer_release is true (set by
// gekko_bridge during the rollback-active part of a round), hook_release
// pushes actors here instead of calling the original wrapper. flush
// runs the original on every entry, in order, at disarm time.
constexpr size_t DEFER_CAP = 2048;
static ManbowActor2D* g_deferred[DEFER_CAP];
static size_t         g_deferred_count = 0;
static bool           g_defer_release  = false;

static inline void g_live_add(ManbowActor2D* a) {
    // Dedupe: with defer-release ON the pool can recycle a slot whose
    // pointer we still hold in g_live. Without this check, every
    // Create-after-Release adds another copy of the same pointer and
    // by-id lookup at load time finds the LATEST actor in N slots.
    for (size_t i = 0; i < g_live_count; ++i) {
        if (g_live[i] == a) return;
    }
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
static SafetyHookInline g_allocate_hook{};
static SafetyHookInline g_release_hook{};

// AllocateActor entry hook. __thiscall, 3 args (this + 2 stack); it
// returns the freshly-pooled ManbowActor2D* in eax — that return value
// IS the actor, so register it. The actor isn't fully wired up yet
// (group / anim_controller get set by the caller right after), but we
// only store the pointer; it is complete long before any save runs.
ManbowActor2D* thiscall hook_allocate(void* self, int a2, int a3) {
    ManbowActor2D* actor =
        (ManbowActor2D*)g_allocate_hook.unsafe_thiscall<int>(self, a2, a3);
    if (actor) g_live_add(actor);
    return actor;
}

int thiscall hook_release(ManbowActor2D* self) {
    if (!self) return g_release_hook.unsafe_thiscall<int>(self);
    if (g_defer_release) {
        // Park the actor on the deferred queue. We do NOT call the
        // original wrapper — so active_flags stays whatever it was
        // (alive), the task vector keeps its SqratFunction refs, and
        // the engine keeps treating the actor as live. Save/load
        // memcpy can then resurrect it without dealing with freed
        // Squirrel state. flush_deferred() runs the real Release on
        // every queued actor at disarm time.
        if (g_deferred_count < DEFER_CAP) g_deferred[g_deferred_count++] = self;
        return 0;
    }
    g_live_remove(self);
    return g_release_hook.unsafe_thiscall<int>(self);
}

} // namespace

// --- Function entry RVAs ----------------------------------------------------
// Verified in IDA against th155 (th155_fresh.exe.i64). If the binary
// updates, re-verify and update these.
//   AllocateActor: Manbow::Actor2DManager::AllocateActor — the single
//                  birth chokepoint for all CreateActor2D* variants.
//   Release:       Manbow::Actor2D::Release.
#define ALLOCATE_ACTOR_ADDR  (0x9E250_R)
#define ACTOR2D_RELEASE_ADDR (0xC1230_R)

namespace live_actors {

void install() {
    g_allocate_hook = safetyhook::create_inline((void*)ALLOCATE_ACTOR_ADDR, (void*)hook_allocate);
    g_release_hook  = safetyhook::create_inline((void*)ACTOR2D_RELEASE_ADDR, (void*)hook_release);

    int ok = g_allocate_hook.enabled() + g_release_hook.enabled();
    log_printf("live_actors::install: %d/2 safetyhook InlineHooks active\n", ok);
}

size_t snapshot(ManbowActor2D** out_buf, size_t max_count) {
    size_t n = g_live_count < max_count ? g_live_count : max_count;
    memcpy(out_buf, g_live, n * sizeof(ManbowActor2D*));
    return n;
}

size_t count() { return g_live_count; }

void set_defer_release(bool on) {
    if (on == g_defer_release) return;
    g_defer_release = on;
    log_printf("live_actors::set_defer_release %s\n", on ? "ON" : "OFF");
}

void flush_deferred() {
    if (g_deferred_count == 0) return;
    log_printf("live_actors::flush_deferred running original Release on %zu actors\n",
               g_deferred_count);
    for (size_t i = 0; i < g_deferred_count; ++i) {
        ManbowActor2D* a = g_deferred[i];
        if (!a) continue;
        g_live_remove(a);
        g_release_hook.unsafe_thiscall<int>(a);
    }
    g_deferred_count = 0;
}

} // namespace live_actors
