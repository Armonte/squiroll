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
// Byte spans (into the last save() dest buffer) of render-tainted pools —
// excluded from the gekko desync checksum, kept in the blob for restore.
int nochecksum_spans(const uint8_t** lo, const uint8_t** hi, int maxn);

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
uint32_t boostpool_save(uint8_t* out, uint32_t cap);
void boostpool_load(const uint8_t* blob, uint32_t len);
// SQUIROLL_BPVALIDATE: verify the rebuilt boost-pool free chains against the
// free_head engine_snap restored. Call AFTER engine_snap::load.
void boostpool_verify(const char* when);
bool validate_enabled();   // SQUIROLL_BPVALIDATE

// Diagnostics for the f=24 va.x rollback bug: set_va_probe publishes the
// player's va.x C++ address; set_load_frame publishes the frame whose blob is
// being restored. boostpool_save/load then log [bpsave]/[bpload]/[bpcover].
void set_va_probe(uint32_t addr);
void set_load_frame(int frame);

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

// DIAGNOSTIC: lean LIVE-SLOT forward-vs-resim diff ([bplive]). Unlike
// diff_locate (RB_DIAG, all-blocks, noisy on free slots), this diffs the
// save() blob (live slots only = what the [sblob] bp checksum hashes), so it
// surfaces the real bp divergence cleanly. Runs unconditionally, frame-gated +
// budget-capped. Reports pool / live-slot real address / field offset.
void diff_live(int frame, int rb);

// DESYNC REPORT ("DesyncUtil", GDC-style): annotated walk of a forward/re-sim
// bp-section pair — names every diverging slot (pool / slot addr /
// desync_registry type) and verdicts each dword KNOWN-RENDER vs UNKNOWN.
// Called from the desync-abort path on the stashed pair so a single run's log
// contains the complete analysis.
void diff_report(const uint8_t* fwd, const uint8_t* re, uint32_t len);

} // namespace battle_pools

#endif // BATTLE_POOLS_H
