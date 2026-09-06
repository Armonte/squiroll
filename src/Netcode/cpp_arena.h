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

// True while a match's rollback session is armed (set_armed(true)). Used to force
// the Ew::sTask layer/effect dispatch synchronous during the rollback window.
bool is_armed();

// Bracket the forward-only render pass (render_one_frame) so its allocations go to
// the arena's separate render region (with SQUIROLL_SYNC_WORKERS) — keeps the
// render-effect connection-node leak out of the sim bump.
void set_render_pass(bool on);

// Render-region dispatch-signal blocks (create_and_bind 0x56AB5): payload
// offsets into the arena, registered at alloc. Forward-only render dispatch —
// snapshot_ring forward-state-pins each one across restore so a rollback never
// rewinds their slot-list head/shared_count to control blocks the game-loop
// thread already released (the 0xEAC9 round-end crash). Returns count written.
int dispatch_signal_offsets(uint32_t* out, int maxn);

// Runtime address of the game-loop (forward-only render-dispatch) ScriptAPI object
// (*0x49AFBC). Its boost::signals2 slot-list shared_count is embedded in the object; the
// re-sim never runs this ScriptAPI, so rolling it back reverts the use_count to a dead
// state -> spurious dispose -> HasPendingFrame abort. snapshot_ring forward-state-pins
// this object across rollback (save on forward capture, re-apply after each restore).
uint32_t gameloop_addr();

// True if this cpp-arena page holds TF4_Number HUD digit geometry (render-derived
// display, excluded from rollback). snapshot_ring skips these in capture/restore and
// in the divergence diagnostic. Pages are registered as numbers update each frame.
bool is_excluded_page(uint32_t pg);

// Crash-time attribution: if addr points into an ALLOCATED arena block, fill
// alloc_rva (the operator-new caller RVA from the block header), reqsize and
// payload base, and return true. Scans backwards for the block header —
// diagnostic-only cost, safe to call from the VEH/fastfail path.
// "sim" / "render" / "none": which cpp_arena region (if any) holds p.
const char* region_of(const void* p);
bool describe_block(uint32_t addr, uint32_t* alloc_rva, uint32_t* reqsize,
                    uint32_t* payload);

// B1 trail determinism: rebuild every active Manbow::TrailLayer ribbon mesh from its
// (deterministic) circular-buffer positions into its persistent cpp-arena vertex
// buffer. Call at the end of each advance (fwd AND re-sim) so the snapshot captures
// identical trail geometry instead of the render-only (forward-only) build.
// SQUIROLL_TRAIL_REBUILD=0 disables (default on). No-op until armed.
void rebuild_trail_meshes();

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
void churn_report(int top);  // log the top allocation sites by freed/live bytes
void advance_frame();        // quarantine clock tick (see cpp_arena.cpp)
bool owns(const void* p);    // is p inside the cpp arena (sim or render region)
uint32_t capacity();    // arena reservation

// Rollback serialization: snapshot_ring holds this across restore/capture so a
// concurrent game-loop/bg-thread RunOneFrame can't walk the arena connection
// lists mid-rewrite. Recursive (CRITICAL_SECTION). See cpp_arena.cpp.
void rollback_lock();
void rollback_unlock();
// Hang diagnostic: who holds g_rollback_cs (0=free) + a label, and the
// game-loop / bg ScriptAPI thread ids. The watchdog prints these.
uint32_t    rollback_cs_owner_tid();
const char* rollback_cs_owner_label();
uint32_t    gameloop_thread_id();
uint32_t    bg_thread_id();
uint32_t    sim_thread_id();
size_t   live_bytes();  // currently-handed-out payload bytes

// DIAGNOSTIC: find which th155 malloc() call produced the block containing
// `addr` and log its call site (RVA). Backed by a ring of recent malloc
// records captured while the rollback session is armed.
void trace_alloc(uint32_t addr);

} // namespace cpp_arena

#endif // CPP_ARENA_H
