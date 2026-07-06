#pragma once
#ifndef RENDER_ARENA_H
#define RENDER_ARENA_H 1

#include <cstddef>
#include <cstdint>

// ============================================================================
// render_arena — a fixed, NON-SNAPSHOTTED heap for plugin/mod render objects.
//
// The rollback snapshot (snapshot_ring) captures + restores the sim arenas
// (sq_arena, cpp_arena, tf4 mspaces). Any object living in those arenas is
// rewound on a rollback. That is correct for SIM state and even for th155's
// OWN render scratch (which the sim references, so its refs must stay valid) —
// but it is FATAL for a plugin's UI render objects: a mod's UI.Core.Text glyph
// / vertex allocations are forward-only, the re-sim reuses those slots, and the
// dangling result gets fed to d3d11 / the GPU driver -> the nvwgf2um
// type-confusion crash that only showed up in dual netplay (solo never loads
// the plugins). See [[project_squiroll_tf4_mspace]] 2026-07-06.
//
// The invariant this enforces: **an object a plugin draws must never live in a
// rolled-back arena.** render_arena is a dedicated region that snapshot_ring
// NEVER registers, so allocations here are never rewound and can never collide
// with the re-sim. Safe precisely because the sim never references plugin UI.
//
// Usage is transparent: squiroll's UI layer brackets plugin render-object
// creation / mutation with a scope; while the scope is active the sim thread's
// arena allocations route here instead of into the snapshot. Plugins do not
// change. An explicit escape hatch (::rollback.unsnapshotted(fn)) is exposed
// for mods that build render state by hand.
// ============================================================================
namespace render_arena {

// Reserve + commit the region. Idempotent; safe to call before the arenas arm.
void  init();
bool  ready();

// Install the th155 UI-allocation hooks (Manbow::String glyph vector +
// Act::BitmapFontResource clone) that bracket those allocations in the scope so
// UI text render memory is never snapshotted. Call once after init(), with the
// th155 image loaded. See render_arena.cpp / task #30.
void  install_ui_hooks();

// Allocator. alloc returns 8-aligned memory; free/realloc accept only pointers
// this arena handed out (guard with owns()). owns() is a fast range check.
void* alloc(size_t n);
void  free(void* p);
void* realloc(void* p, size_t n);
bool  owns(const void* p);

// --- unsnapshotted scope ----------------------------------------------------
// While depth > 0 on the sim thread, cpp_arena / tf4 route the sim thread's
// allocations here. Nestable. enter/leave must be balanced (RAII Scope below).
void  enter();
void  leave();
bool  in_scope();

struct Scope { Scope() { enter(); } ~Scope() { leave(); } };

// --- Squirrel plugin sub-domain scope (M3) -----------------------------------
// A SEPARATE depth from enter()/leave(). While in_sq_scope() > 0 on the sim
// thread, sq_arena's VM-allocator hooks route Squirrel allocations here too (not
// just cpp_arena's native allocs). Kept distinct on purpose: front-render uses
// enter()/leave() for NATIVE routing and must NOT have its Squirrel allocations
// rerouted — only the plugin bracket sets the Squirrel scope. Plugin execution
// (async modifier ctor + Update) is bracketed with plugin_scope_enter/leave,
// which bump BOTH scopes so a plugin's native (String/glyph/VB) AND Squirrel
// (the Text wrapper + its churn) allocations all land in render_arena and stay
// mutually consistent (a rewound wrapper pointing at a forward-only String would
// dangle — so both must be forward-only, not just the native side).
void  sq_enter();
void  sq_leave();
bool  in_sq_scope();

// Combined plugin bracket = enter()+sq_enter() / sq_leave()+leave(). Balanced.
void  plugin_scope_enter();
void  plugin_scope_leave();
struct PluginScope { PluginScope() { plugin_scope_enter(); } ~PluginScope() { plugin_scope_leave(); } };

// Diagnostics
size_t bytes_live();
size_t bytes_capacity();

} // namespace render_arena

#endif
