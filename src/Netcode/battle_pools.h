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

// Re-home every live AnimationController2D's three CompositeSprite
// std::vector backing buffers into cpp_arena, at a fixed 256-element
// capacity. The controllers are built by vs.Initialize (before cpp_arena
// is armed) so their vector buffers sit uncaptured on the CRT heap; a
// rollback re-sim then runs on stale buffers (the `0xB9196` crash). Call
// once at session arm, AFTER cpp_arena::set_armed(true), so the fresh
// buffers land in the captured arena and never realloc mid-match.
void reserve_anim_vectors();

// Serialize every battle pool (block bytes + allocator structs + the
// actor-manager + ID counters) into out[0..cap). Returns bytes written,
// 0 on overflow.
uint32_t save(uint8_t* out, uint32_t cap);

// Restore from a save() blob — memcpy every block back to its (stable)
// address and restore the allocator structs + counters.
void load(const uint8_t* blob, uint32_t len);

// Snapshot/restore the Manbow Sqrat math boost::pools (SqVector3 = each
// actor's this.va/vf/vfBaria velocity vectors, plus SqMatrix/SqIndexVector3).
// These draw blocks from g_tf4_mspace at ~0x19Dxxxxx — outside every snapshot
// arena and not TF4 TPoolAllocators, so save()/g_pool_rva[] never captured
// them. Leaving this.va.x un-snapshotted caused the f=15 depth-1 walk-vs-stand
// divergence. Whole-block capture (the pool struct itself is in .data, covered
// by engine_snap). See battle_pools.cpp for the full rationale.
uint32_t mathpool_save(uint8_t* out, uint32_t cap);
void mathpool_load(const uint8_t* blob, uint32_t len);

// DIAGNOSTIC: log a per-pool checksum line, tagged. Comparing the forward
// vs re-sim line shows which specific pool first diverges.
void log_fingerprint(const char* tag);

// DIAGNOSTIC: forward-vs-resim per-offset divergence locator. Call once per
// Advance at adv-top with the gekko frame number and rolling_back flag. On a
// forward advance (rb=0) it stores the suspect pools' bytes keyed by frame;
// on a re-simulation (rb!=0) of the same frame it diffs and logs the first
// diverging pool / slot / field offset, then stops. In a solo
// GekkoStressSession the two runs have identical inputs, so any difference is
// pure non-determinism — the field offset maps straight to a struct member.
void diff_locate(int frame, int rb);

} // namespace battle_pools

#endif // BATTLE_POOLS_H
