#pragma once

#ifndef BULLET_ARENA_H
#define BULLET_ARENA_H 1

#include <stdint.h>
#include <stddef.h>

// bullet_arena — th155's Bullet physics heap, captured for rollback.
//
// th155 embeds Bullet. Every Bullet allocation (collision shapes, the
// collision world, the broadphase, contact manifolds, and the per-sprite
// Manbow::ActorCollisionData hitbox shapes) funnels through Bullet's global
// allocator function pointers:
//     0x498D44  btAllocFunc        -> 0x1E79C0  (malloc wrapper)
//     0x498D48  btFreeFunc         -> 0x1E79D0  (free wrapper)
//     0x498D4C  btAlignedAllocFunc -> 0x1E7930  (delegates to [0x498D44])
//     0x498D50  btAlignedFreeFunc  -> 0x1E7980  (delegates to [0x498D48])
// The aligned alloc/free DELEGATE to the base [0x498D44]/[0x498D48] pair, so
// overwriting just those two pointers routes 100% of Bullet's heap into a
// fixed size-classed arena (same model as sq_arena / cpp_arena).
//
// Why this is needed: the rollback snapshot captures the AnimCtrl2D pool
// (which holds the ActorCollisionData* vector) but NOT the Bullet shapes the
// ActorCollisionData objects embed — those were malloc'd, outside every
// captured region. On a rollback re-sim the shape graph is stale, and
// AppendSpriteRange dereferences a NULL ActorCollisionData* -> the #1
// rollback crash (0xB9196). Routing Bullet into a captured arena makes the
// whole physics heap part of the snapshot and bit-reproducible.
//
// Hooking th155's Bullet allocator does not touch squiroll/GekkoNet: those
// have their own CRT and never call th155's Bullet alloc pointers.

namespace bullet_arena {

// Overwrite the two base Bullet allocator function pointers. Call once
// during squiroll init (common_init), before th155 creates any Bullet
// object, so the entire physics heap lands in the arena.
void install();

// Raw image of the arena [base, base+used). Returns bytes written, 0 on
// overflow.
uint32_t save(uint8_t* out, uint32_t cap);

// Restore the raw image — allocator state and every block in one memcpy.
void load(const uint8_t* blob, uint32_t len);

uint8_t* base();        // arena base — the MEM_WRITE_WATCH region
uint32_t used();        // high-water bytes (what save() writes)
uint32_t capacity();    // arena reservation
size_t   live_bytes();  // currently-handed-out payload bytes

} // namespace bullet_arena

#endif // BULLET_ARENA_H
