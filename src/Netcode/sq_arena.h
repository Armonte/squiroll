#pragma once

#ifndef SQ_ARENA_H
#define SQ_ARENA_H 1

#include <stdint.h>
#include <stddef.h>

// Squirrel VM heap arena.
//
// Rollback needs the entire Squirrel VM heap as one snapshottable unit. We
// redirect the VM's allocator (sq_malloc/realloc/free) into a single fixed
// pre-reserved region — the "arena" — so the whole VM object graph lives in
// [base(), base()+used()) and a snapshot is one memcpy. Addresses are stable
// (the region never moves), so raw pointers inside the snapshot stay valid
// on restore, and re-allocation is deterministic (the allocator's own state
// — bump cursor + free-lists — lives at the head of the arena and is
// captured/restored by the same memcpy).
//
// Only Squirrel VM allocations are routed here; non-VM allocations (Boost,
// Concurrency, the engine) keep using the real CRT heap.

namespace sq_arena {

// Reserve the arena and redirect Squirrel VM allocations into it.
// [sq_code_lo, sq_code_hi) is the Squirrel VM code address range — an
// allocation whose caller's return address lies in it is a VM allocation.
// Call once, before the Squirrel VM is created (sq_open).
void install(uintptr_t sq_code_lo, uintptr_t sq_code_hi);

// Snapshot window. The live arena is exactly [base(), base()+used()):
// the allocator metadata header followed by every block ever bump-allocated
// (freed blocks stay in place on a free-list). memcpy this range to save;
// memcpy it back to restore. used() only ever grows.
uint8_t* base();
uint32_t used();
uint32_t capacity();

// Serialize [base, base+used) into out[0..cap). Returns bytes written,
// 0 on overflow. The blob begins with the allocator metadata header, so
// load() restores the bump cursor + free-lists along with the whole VM
// heap — re-allocation after a restore is bit-deterministic.
uint32_t save(uint8_t* out, uint32_t cap);
bool     load(const uint8_t* blob, uint32_t len);

// Diagnostics.
bool   installed();
size_t live_bytes();   // bytes currently handed out (excludes free-listed)

} // namespace sq_arena

#endif // SQ_ARENA_H
