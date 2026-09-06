#pragma once

#ifndef SNAPSHOT_RING_H
#define SNAPSHOT_RING_H 1

#include <stdint.h>

// snapshot_ring — dirty-page rollback snapshot for the three big arenas
// (sq_arena, bullet_arena, cpp_arena).
//
// Dumping the whole arena every frame copied ~21 MB per save, ~10x per
// displayed frame in the stress rig — memory-bandwidth bound, ~3 fps. Almost
// all of that is IMMUTABLE during a match (compiled Squirrel bytecode, class
// definitions, collision-shape templates). Dirty-page tracking snapshots only
// the pages a frame actually wrote.
//
// Each arena is VirtualAlloc'd with MEM_WRITE_WATCH. A capture reads the
// write-watch (pages dirtied since the last capture), stores each dirty page's
// PRE-image (its value before this frame, taken from a running mirror), then
// syncs the mirror. A rollback walks that reverse-delta chain backwards —
// cost proportional to what CHANGED (KB), not heap size.
//
// The sblob non-arena sections (battle_pools, engine_snap, input) are
// ~1 MB combined; the caller hands them in as one opaque blob per frame
// and snapshot_ring rings them in full — not worth page-tracking. cpp_arena
// is now page-tracked alongside sq_arena and bullet_arena.
//
// The GekkoNet save blob shrinks to just the frame number (the ring handle);
// the real state lives here.

namespace snapshot_ring {

// Set up the mirrors + write-watch baseline. Call once at session arm, after
// the arenas are installed and the battle pools pre-grown, before the first
// advance/save.
void arm();
bool armed();
// True when SQUIROLL_DIAG is set: heavy per-frame divergence tracing is on.
// Fast mode (default) skips [cspart]/[comp]/[divf]/cppb + gekko [sblob]/diff_locate.
bool diag_on();

// Capture frame `frame`: store the three arenas' dirty-page deltas and a copy
// of `sblob` (the serialized non-arena sections). Returns the full-state
// desync checksum.
// The RAW page+blob checksum capture() returns costs ~800us per save: it builds a
// compacted copy of the whole small blob (skipping the nochecksum spans) and FNV-
// hashes it, then folds every in-use page hash. DUAL netplay throws that value
// away — the cross-peer checksum is the structural one from save_battle, because
// a raw hash is pointer-contaminated between processes. Tell capture() when the
// result is actually wanted so it can skip the work entirely.
void set_want_raw_checksum(bool on);

uint32_t capture(uint32_t frame, const uint8_t* sblob, uint32_t sblob_len, uint32_t nocsum_tail = 0);

// Roll the arenas back to `frame` and return that frame's sblob blob (its
// length in *sblob_len). Returns nullptr if `frame` is outside the ring
// window (a hard error — logged).
const uint8_t* restore(uint32_t frame, uint32_t* sblob_len);

// Snapshot the game-loop (forward-only render-dispatch) ScriptAPI's reachable arena graph
// as the live forward copy. Call at the STABLE end of that ScriptAPI's RunOneFrame (balanced
// slot-list refcount). The graph is re-applied after every rollback restore so this render
// state never rolls back into an inconsistent state (the ~f40-70 shared_count abort).
// No-op unless SQUIROLL_GL_PIN is set and the session is armed.
void gl_capture();

// Cross-peer diagnosis: per-component sub-hashes from the last fold_checksum
// (sq_arena / bullet_arena / small blob). Comparing these between two peers at
// the same gekko frame localises a desync to one component. Any out-ptr may be
// null.
void last_subchecksums(uint32_t* sq, uint32_t* bt, uint32_t* sblob);

} // namespace snapshot_ring

#endif // SNAPSHOT_RING_H
