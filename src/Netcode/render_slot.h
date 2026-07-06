#pragma once
#ifndef RENDER_SLOT_H
#define RENDER_SLOT_H 1

// ============================================================================
// render_slot — a squiroll-OWNED th155 Manbow::DrawCommandSlot for plugin HUDs.
//
// THE PROBLEM (task #30, see [[project_squiroll_tf4_mspace]] 2026-07-06):
// A plugin (ping_display) draws by connecting its UI.Core.Text objects to
// th155's shared ::graphics.slot.ui — a Manbow::DrawCommandSlot whose
// boost::signals2 grouped signal lives in the rolled-back cpp_arena. The
// plugin's *connection nodes* are allocated into render_arena (NOT rolled back,
// via the battle.nut _SetupModifiers plugin scope). A rollback then rewinds the
// signal's list head/links (arena side) while the connection nodes persist
// (render_arena side) -> th155's render-time list walk
// (Act::ScriptAPI::RunOneFrame @0x2FAD0 -> boost_signals2_invoke_slots
// @0x31250) hits a half-rewound circular list and loops forever -> HANG.
//
// THE FIX:
// Give the plugin its OWN DrawCommandSlot whose ENTIRE object graph (slot,
// signal, connlist, container, CRITICAL_SECTION) lives in render_arena, so
// nothing about it is ever rewound. th155 never walks this slot — only WE do,
// FORWARD-ONLY, from the render thread (overlay_draw), replicating th155's
// per-node invoke (boost_signals2_invoke_one_slot @0x31CC0) minus the
// round-robin cursor and node-erase that cause the divergence/hang.
//
// All offsets verified against th155_fresh.exe.i64 (see render_slot.cpp).
// ============================================================================
namespace render_slot {

// Create the slot ONCE, pre-arm, on the SIM thread (the Squirrel VM owner),
// inside a render_arena::Scope. Idempotent; self-guards on the th155 script VM
// being up (::graphics.slot table created) and render_arena being ready — so it
// is safe to call every frame; it no-ops after the first success and returns
// false (retry) until the VM exists. Binds the slot into Squirrel as
// ::graphics.slot.<NAME> (and ::<NAME>). Returns true once the slot exists.
bool init();

// True once the slot has been created (native pointer resolved).
bool ready();

// Walk the slot's boost::signals2 connection list FORWARD-ONLY and invoke each
// CONNECTED draw callback. Call from the RENDER thread (overlay_draw). Performs
// NO allocation and NO list mutation, so it is rollback-safe. No-op until init()
// has succeeded.
void emit();

// Native Manbow::DrawCommandSlot* (diagnostics); null until init() succeeds.
void* native_slot();

} // namespace render_slot

#endif // RENDER_SLOT_H
