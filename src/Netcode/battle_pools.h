#pragma once

#ifndef BATTLE_POOLS_H
#define BATTLE_POOLS_H 1

#include <stdint.h>

// Battle object pools — the C++ half of the rollback snapshot.
//
// th155's battle C++ objects (ManbowActor2D and every projectile/effect,
// the actor groups, cameras, animation controllers, aura/afterimage/sensor)
// are drawn from TF4::TPoolAllocator pools — each a chain of heap blocks of
// fixed-size slots. Blocks are never freed mid-match. We pre-grow the pools
// at battle start so the block set is frozen, then snapshot/restore every
// block as raw memory.
//
// This must be captured in the SAME consistent unit as the Squirrel VM
// arena (sq_arena): the C++ objects here hold Squirrel references (sq_obj,
// SqratFunctions) and the VM objects hold C++ references — rolling back one
// without the other desyncs refcounts.

namespace battle_pools {

// Pre-grow every battle pool so its block set is fixed for the match.
// Call once when the rollback session arms (at Round_Fight).
void pregrow();

// Serialize every battle pool (block bytes + allocator structs + the
// actor-manager + ID counters) into out[0..cap). Returns bytes written,
// 0 on overflow.
uint32_t save(uint8_t* out, uint32_t cap);

// Restore from a save() blob — memcpy every block back to its (stable)
// address and restore the allocator structs + counters.
void load(const uint8_t* blob, uint32_t len);

} // namespace battle_pools

#endif // BATTLE_POOLS_H
