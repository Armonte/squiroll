#pragma once

#ifndef CPP_ARENA_H
#define CPP_ARENA_H 1

#include <stdint.h>
#include <stddef.h>

// Engine C++ battle-state arena — the C++ half of the rollback snapshot.
//
// The mirror of sq_arena. sq_arena gave the Squirrel VM heap a fixed,
// address-stable, snapshottable region; cpp_arena does the same for the
// engine's C++ battle allocations. th155's battle satellites — actor task
// nodes (operator new(0x38)), the scheduler's std::list/map/vector nodes,
// every std::vector / std::shared_ptr control block — are all raw
// `operator new` (CRT heap), never pooled and never at reproducible
// addresses. Rolling back the Squirrel arena while these dangle is the
// use-after-free the #64 attempt hit.
//
// We interpose `operator new` and the universal free chokepoint
// (__free_base). Unlike sq_arena (which gates by the caller's return
// address — every VM alloc has a caller in the Squirrel code range),
// the C++ engine has no clean code band, so cpp_arena gates by an
// explicit thread-local flag: while sim-active is set on a thread, that
// thread's `operator new` calls route into the arena. The flag is set
// ONLY around the deterministic battle sim (advance_one_frame's engine
// calls), so exactly the battle-frame allocations land in the arena and
// nothing else does. free routes by address range (exact, no flag).
//
// The arena is captured/restored as one contiguous region alongside the
// Squirrel arena — together they make the cross-coupled C++<->Squirrel
// object graph one address-stable, snapshottable unit.

namespace cpp_arena {

// Reserve the arena and interpose operator new / __free_base. Call once,
// early (common_init), before any battle. Idempotent.
void install();

// Sim-active gate. While set on the calling thread, that thread's
// `operator new` allocations are routed into the arena. Set true
// immediately before the battle-sim engine calls and false immediately
// after — see advance_one_frame.
void set_sim_active(bool on);
bool sim_active();

// Snapshot window. The live arena is exactly [base(), base()+used()):
// the allocator metadata header followed by every block ever
// bump-allocated. memcpy this range to save, memcpy it back to restore.
uint8_t* base();
uint32_t used();
uint32_t capacity();

// Serialize [base, base+used) into out[0..cap). Returns bytes written,
// 0 on overflow.
uint32_t save(uint8_t* out, uint32_t cap);

// Restore a save() blob: memcpy it back over the arena head. The blob's
// first bytes are the allocator metadata, so this restores the bump
// cursor + free-lists along with every block. Returns false on a bad
// length.
bool load(const uint8_t* blob, uint32_t len);

// Diagnostics.
bool   installed();
size_t live_bytes();   // bytes currently handed out (excludes free-listed)

} // namespace cpp_arena

#endif // CPP_ARENA_H
