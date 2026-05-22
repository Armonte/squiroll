#pragma once

#ifndef INPUT_HIST_H
#define INPUT_HIST_H 1

#include <stdint.h>

// input_hist — captures th155's per-player input-history objects.
//
// Each player has an InputHistory object (count @ obj+0x00, a std::vector<u16>
// ring {begin@+0x10, storage-end@+0x14}) that a Squirrel input native appends
// to once per frame (input_history_u16__append @ 0x169A20). The vector grows
// by doubling. These objects live on th155's general heap and are captured by
// NO other snapshot section — so a rollback re-sim ran on a stale count +
// vector header, reallocated the vector at a different frame than the forward
// run, and desynced (DESYNC frame=11, cpp_arena bump divergence).
//
// This module hooks the append to discover the objects, pre-grows each vector
// to a fixed capacity at session arm so it never reallocates again (the
// backing buffer then has a stable address), and snapshots object + buffer
// per frame as one section in the rollback blob.

namespace input_hist {

// Hook input_history_u16__append for object discovery. Call once at init,
// before the battle starts appending.
void install();

// Pre-grow every discovered InputHistory vector to a fixed capacity so it
// never reallocates mid-match. Call once at session arm.
void pregrow();

// Snapshot / restore every discovered object + its vector buffer.
uint32_t save(uint8_t* out, uint32_t cap);
void     load(const uint8_t* blob, uint32_t len);

} // namespace input_hist

#endif // INPUT_HIST_H
