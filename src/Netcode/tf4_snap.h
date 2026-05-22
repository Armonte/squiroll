#pragma once

#ifndef TF4_SNAP_H
#define TF4_SNAP_H 1

#include <stdint.h>

// tf4_snap — rollback dirty-page tracker for th155's TF4-engine heap.
//
// th155's TF4 engine heap is a dlmalloc/mspace (the global pointer
// dword_4DB48C caches its address; a second one lives embedded at
// .data:0x4DC0C0). It backs itself with raw VirtualAlloc segments — so
// none of the snapshot arenas (sq_arena / bullet_arena / cpp_arena, which
// hook the Squirrel allocator / Bullet / operator new) capture it. Battle
// objects allocated through this heap — e.g. the Squirrel-instance object
// pool whose free-list head lives at 0x4DCD00 — therefore diverge on a
// rollback re-sim: a "free" pool node still holds forward-run user data,
// the next pop reads it as a free-list link and dereferences garbage.
//
// This heap is th155's general engine heap and the whole engine churns it
// — measured ~14 MB of dirty pages per frame. A 32-bit process cannot
// afford a 14 MB/frame raw delta ring, so tf4_snap:
//   1. patches the mspace's four VirtualAlloc call sites to add
//      MEM_WRITE_WATCH, so dirty pages are found by a cheap GetWriteWatch
//      rather than per-page faults (a segment that predates the patch
//      falls back to a mirror scan);
//   2. keeps one full mirror per segment;
//   3. stores deltas COMPRESSED — only the byte-runs that actually differ
//      from the mirror (a written 4 KB page typically changes far less),
//      not whole pages.
// A rollback reverse-applies that byte-run chain.
//
// Driven alongside snapshot_ring: arm() at session arm, capture() per
// save, restore() per rollback.

namespace tf4_snap {

// Patch the mspace's VirtualAlloc call sites to add MEM_WRITE_WATCH. Call
// once at squiroll init (common_init) — as early as possible, so the
// mspace's segments are write-watch-enabled from birth.
void install();

// Resolve the mspace, walk its VirtualAlloc segment list, mirror every
// segment and start the delta ring. Call at session arm, after
// snapshot_ring::arm(), before the first advance/save.
void arm();
bool armed();

// Finalise frame `frame`: diff every dirty segment page against its
// mirror, store the changed byte-runs as that frame's reverse delta, and
// pick up any segment the mspace grew during the frame.
void capture(uint32_t frame);

// Roll the mspace segments back to `frame` by reverse-applying the
// byte-run pre-image chain. No-op (logged) if `frame` is outside the ring.
void restore(uint32_t frame);

} // namespace tf4_snap

#endif // TF4_SNAP_H
