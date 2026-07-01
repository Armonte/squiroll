#pragma once

#ifndef TF4_ARENA_H
#define TF4_ARENA_H 1

#include <stdint.h>

// tf4_arena — captures th155's two engine-private dlmalloc "TF4 mspace" pools
// for rollback.
//
// tf4_mspace_create (RVA 0x331C0, called once from engine init) VirtualAllocs
// two fixed regions — a ~128 MB primary and a ~32 MB secondary — and builds a
// create_mspace_with_base dlmalloc allocator inside each (footprint_limit ==
// the region size, so the pools can NEVER grow past their initial region). The
// malloc_state lives INSIDE the region, so snapshotting the whole region
// snapshots the allocator state atomically.
//
// These pools hold objects the sim references but that live OUTSIDE the three
// snapshotted arenas (sq/bullet/cpp): boost::signals2 control blocks +
// connection bodies, Sqrat holder objects, the NetworkNode/Number pool. Because
// references cross between the snapshotted arenas and these pools, a rollback
// that rewinds the arenas but not the pools leaves the object graph half-rewound
// -> the reproducible 0xC0000005 crash. Snapshotting the pools closes that gap.
//
// This module runs at the EARLIEST init point (common_init, at th155's
// entrypoint, before engine init) and IAT-patches th155's VirtualAlloc import
// so the two pool regions are allocated with MEM_WRITE_WATCH (needed for the
// dirty-page snapshot) at fixed bases (run-to-run determinism gravy). It records
// each region's {base,size} for snapshot_ring to register as rollback arenas.

namespace tf4_arena {

// IAT-patch th155's VirtualAlloc import. MUST run BEFORE the game's engine init
// calls tf4_mspace_create (RVA 0x331C0). Idempotent. Honors SQUIROLL_NO_TF4SNAP.
void early_install();

// True iff BOTH pool regions were intercepted (and the module is neither
// disabled by the env kill-switch nor too-late). snapshot_ring::arm() registers
// the tf4 arenas only when this is true.
bool ready();

// Region i base / size. i == 0 -> region A (~128 MB primary), i == 1 -> region B
// (~32 MB secondary). Returns nullptr / 0 for an un-intercepted region.
uint8_t* base(int i);
uint32_t size(int i);

} // namespace tf4_arena

#endif // TF4_ARENA_H
