#pragma once

#ifndef TREE_WALKER_GUARD_H
#define TREE_WALKER_GUARD_H 1

// Defensive replacement for std__map_string_int__lower_bound at 0x14B00
// (the boost::signals2::detail::grouped_list's RB-tree walker).
//
// The original loops on `node = node->left|right` until `node->_Isnil`
// becomes true. If the tree gets corrupted (a child pointer becomes
// non-canonical or a node memory page is zeroed) the walk faults at
// every read site; crash_handler's universal NULL-skip absorbs each
// AV but does NOT update `eax` (the node pointer), so the loop spins
// forever — the game freezes with one CPU core pegged. Universal skip
// reports ~13K hits/run normally, with rare spins to infinity.
//
// This module installs a SafetyHookInline at the function entry that
// runs a bounded reimplementation. Comparison logic exactly matches
// the original asm at 0x14B16..0x14B3D; iterations are capped at
// kMaxSteps; non-canonical node pointers and zombie pages bail to
// returning the current parent (= "key not found", lower_bound
// sentinel). For a healthy tree this returns identical results; for a
// corrupted one the walk terminates with at worst a missed lookup,
// which is vastly preferable to a hang.

namespace tree_walker_guard {

void install();

} // namespace tree_walker_guard

#endif // TREE_WALKER_GUARD_H
