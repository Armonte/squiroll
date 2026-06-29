#pragma once

#ifndef ACTOR2D_LOG_H
#define ACTOR2D_LOG_H 1

// Diagnostic: SafetyHookInline on Manbow::Actor2D::UpdateChildMatrices
// (0xC2B40). Per-call dump of count_this vs count_child so the f=15 sq+bt
// rollback divergence (branch flips between createProxy and setAabb) can
// be attributed to a specific Actor2D.
//
// install() is idempotent (the SafetyHookInline is static). Call once from
// common_init, alongside live_actors::install().

namespace actor2d_log {

void install();

// Arm a Dr0 hardware write-watch on `addr` (the sim thread); the VEH logs each
// write's value + EIP/rva + frame/rb/depth. Used to name the C++ writer of the
// player's va.x at the f=24 divergence. Arms once.
void watch_arm(uint32_t addr);

} // namespace actor2d_log

#endif // ACTOR2D_LOG_H
