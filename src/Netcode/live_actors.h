#pragma once

#ifndef LIVE_ACTORS_H
#define LIVE_ACTORS_H 1

// Tracks every live ManbowActor2D in the game by hooking
// Manbow::Actor2DManager::AllocateActor (registration — the single
// chokepoint all five CreateActor2D* variants funnel through) and
// Manbow::Actor2D::Release (unregistration).
//
// Backs the GekkoNet save_state / load_state path — we need to be able to
// enumerate all live actors without going through Squirrel for performance
// and to avoid VM-state coupling during rollback resimulation.
//
// NOT THREAD-SAFE — game loop is single-threaded. If you start touching this
// from a worker, add a spinlock.

#include <stdint.h>
#include "TF4.h"   // ManbowActor2D

namespace live_actors {

// One-shot install of the lifecycle hooks. Call from common_init after
// patch_netplay() so the binary is hot-patchable.
void install();

// Snapshot the current set into a contiguous array. Returns count.
// out_buf must hold at least max_count pointers. Pointers are stable until
// the next CreateActor2D / Release call in the game loop.
size_t snapshot(ManbowActor2D** out_buf, size_t max_count);

// Cheap accessor for size — useful for sizing the save buffer.
size_t count();

// Deferred-release control. When set true (by gekko_bridge during a
// rollback-active round), hook_release stops calling the original
// Manbow::Actor2D::Release and instead pushes the actor onto a deferred
// queue. The actor stays in the live registry, active_flags untouched,
// task vector still holding its SqratFunctions — so rollback can
// restore it via state memcpy without resurrecting freed memory.
// flush_deferred() actually invokes the original Release on every
// queued actor (call at round end / disarm).
void set_defer_release(bool on);
void flush_deferred();

} // namespace live_actors

#endif // LIVE_ACTORS_H
