#pragma once

#ifndef TF4_POOL_H
#define TF4_POOL_H 1

// tf4_pool — re-home th155's TF4 Squirrel-instance object pool so it can be
// rollback-snapshotted.
//
// th155 backs Squirrel class instances with C++ objects handed out by a
// fixed-size object pool (free-list head at .data:0x4DCD00; pop = sub_45710,
// grow = SQVM__Call_0 / sub_45D20). The pool grows by carving slabs from
// th155's TF4-engine heap — a dlmalloc/mspace at .data:0x4DC0C0. Those
// slabs hold the pool's free-list nodes, so a rollback that does not also
// roll the slabs back leaves a stale free list and the next pop derefs
// garbage (crash in sub_45710).
//
// The mspace itself CANNOT be rollback-snapshotted: it is th155's general
// engine heap, and the audio thread keeps live Ogg/Vorbis decoder state in
// it — rolling the whole mspace back under the running audio thread
// corrupts the decoder (crash in its memmove). So instead of snapshotting
// the mspace, tf4_pool redirects just the pool's slab allocation: the one
// `call TF4__MeshVertex__PoolAlloc` inside SQVM__Call_0 is patched to draw
// from cpp_arena, which IS rollback-snapshotted. The pool's free-list head
// is in .data (captured by engine_snap); with its slabs in cpp_arena the
// whole pool now rolls back, and the mspace is left untouched.

namespace tf4_pool {

// Patch the pool's slab-allocation call site, and install the objpool
// grow-hook that auto-discovers the generic-grow battle pools. Call once at
// squiroll init (common_init) — before any battle Squirrel instance is
// created.
void install();

// Freeze the generic-grow battle object pools (the Squirrel-instance pool,
// effect-actor pool, mesh pools, ...) so they never grow mid-match. Call at
// session arm, on the sim thread, while cpp_arena is armed and AFTER any
// pool the match needs has been discovered — i.e. right next to
// battle_pools::pregrow(). Prevents the in-rollback-window tf4_objpool_grow
// sorted-free-list-insert that spins/corrupts (the f=34 hang).
void pregrow_objpools();

} // namespace tf4_pool

#endif // TF4_POOL_H
