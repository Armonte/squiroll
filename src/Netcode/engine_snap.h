#pragma once

#ifndef ENGINE_SNAP_H
#define ENGINE_SNAP_H 1

#include <stdint.h>

// Engine scheduler fixed-region snapshot — the last piece of the rollback
// state, alongside sq_arena (Squirrel VM heap), cpp_arena (engine C++
// allocations) and battle_pools (battle object pools).
//
// The per-frame task scheduler is a small set of fixed-address objects,
// all constructed at game init (verified in IDA):
//   * Act::ScriptAPI         — operator new(0x108) in __init_game_squirrel_
//                              related, pointer stored at *0x4DACFC. Holds
//                              four std::list members (task lists).
//   * the RunOneFrame driver — built once via the Sqrat NetworkClient
//                              binding, pointer at *0x49B01C.
// Their std::list sentinel nodes are allocated inside those constructors
// (also game init) and never move. Per-frame the lists' cursors, size
// counters and sentinel next/prev links mutate; the list element NODES
// are operator new'd during the sim and are already captured by cpp_arena.
//
// So the scheduler's rollback state is exactly: the two objects + their
// sentinel nodes + two global scalars (frame counter, Actor2D task-id
// counter) — all at addresses that are stable for the whole battle. This
// module snapshots them as a self-describing list of fixed regions.

namespace engine_snap {

// Serialize the scheduler fixed regions into out[0..cap). The blob is
// self-describing ([magic][count] then per region [addr][len][bytes]),
// so load() restores without re-resolving anything. Returns bytes
// written, 0 on overflow.
uint32_t save(uint8_t* out, uint32_t cap);

// Restore from a save() blob — memcpy each recorded region back to its
// address. Each region is range-checked before the write.
void load(const uint8_t* blob, uint32_t len);

} // namespace engine_snap

#endif // ENGINE_SNAP_H
