#pragma once

#ifndef CPP_ARENA_H
#define CPP_ARENA_H 1

#include <stdint.h>
#include <stddef.h>

// cpp_arena — th155's C++ heap, captured for rollback. One of the four
// rollback snapshot components: sq_arena (the whole Squirrel subsystem),
// battle_pools (TF4 TPoolAllocator battle objects), cpp_arena (this) and
// engine_snap (fixed scheduler objects).
//
// th155's battle objects own dynamically-allocated C++ memory that no other
// component captures: std::list task-list nodes, and — the proven cause of
// the within-peer rollback divergence — the std::vector backing buffers of
// AnimationController2D/Dynamic and Actor2DGroup. All of it is allocated
// through th155's scalar `operator new` (0x2E15AB). cpp_arena inline-hooks
// that operator new and routes every allocation into a fixed size-classed
// arena (mirroring sq_arena's design), and range-routes `_free_base`
// (0x312347) so frees of arena pointers recycle within the arena.
//
// The snapshot is a raw image of [base, base+used) — which includes the
// arena's own allocator metadata (bump pointer + per-class free lists), so
// allocation/free is bit-reproducible across a rollback re-simulation and
// every battle object gets a STABLE address. That stability is what fixes
// the divergence: a std::vector buffer is re-allocated at the same address
// on re-sim instead of wherever the CRT heap happened to put it.
//
// Hooking th155.exe's operator new does not affect squiroll/GekkoNet —
// Netcode.dll has its own statically-linked CRT — so the arena only ever
// holds th155's allocations and stays battle-scoped.

namespace cpp_arena {

// Install the operator-new (alloc) + _free_base (free) inline hooks. Call
// once during squiroll init (common_init), before th155 builds battle
// objects.
void install();

// Raw image of the arena [base, base+used). Returns bytes written, 0 on
// overflow.
uint32_t save(uint8_t* out, uint32_t cap);

// Restore the raw image — allocator state and every block in one memcpy.
void load(const uint8_t* blob, uint32_t len);

// Arm/disarm. While armed, operator new is routed into the arena; outside
// a match it goes to the real allocator (keeps the snapshot battle-scoped).
void set_armed(bool on);

// Designate the simulation thread. Call once from the battle/game thread
// before snapshot_ring::arm(); afterwards only that thread's operator new is
// routed into the arena (background threads — audio — use the real heap, so
// their non-deterministic allocation never churns the snapshot).
void set_sim_thread(uint32_t tid);

// True iff the current thread is the simulation thread (and one has been
// registered). Used by tf4_pool's redirect to keep non-sim-thread pool
// slabs (Ogg/FMOD stream readers, etc.) out of the snapshot.
bool is_sim_thread();

// Mark a rollback re-simulation advance in progress. While set, a free of a
// real-heap (non-arena) block is suppressed — the real Win32 heap is not
// snapshotted, so the forward run already freed it and re-freeing would
// double-free (STATUS_HEAP_CORRUPTION).
void set_resim(bool on);

// Read accessor for the same flag. Other guards use this to no-op
// real-heap mutations during re-sim (e.g. boost::signals2 connection
// inserts that would otherwise corrupt the heap-resident grouped_list
// state which is not part of any tracked arena).
bool is_resim();

// Raw arena allocation, bypassing the operator-new routing. nullptr on
// overflow. See tf4_pool — re-homes th155's Squirrel-instance object pool.
// caller_abs: the absolute address of the real (th155) call site, so
// attribute() names it instead of the redirect shim; 0 = use _ReturnAddress.
void* raw_alloc(uint32_t n, uint32_t caller_abs);

// DIAGNOSTIC: log which allocated block owns arena byte-offset `off`, and
// the RVA of the code that allocated it — attributes a rollback divergence
// to a subsystem. See cpp_arena.cpp.
void attribute(uint32_t off);

// DIAGNOSTIC: per-frame arena alloc/free sequence trace. trace_reset()
// starts a fresh recording; trace_check(frame) saves the forward run's
// sequence and diffs a re-sim of the same frame against it. See cpp_arena.cpp.
void trace_reset();
void trace_check(uint32_t frame, int rb);

// DIAGNOSTIC (Phase 1): log + reset the per-advance count of the residual
// render (DrawCommandSlot) / boost::signals2 allocators. Call once per advance.
void diag_alloc_counts(int frame, int rb);

uint8_t* base();        // arena base — the MEM_WRITE_WATCH region
uint32_t used();        // high-water bytes (what save() writes)
uint32_t capacity();    // arena reservation
size_t   live_bytes();  // currently-handed-out payload bytes

// DIAGNOSTIC: find which th155 malloc() call produced the block containing
// `addr` and log its call site (RVA). Backed by a ring of recent malloc
// records captured while the rollback session is armed.
void trace_alloc(uint32_t addr);

} // namespace cpp_arena

#endif // CPP_ARENA_H
