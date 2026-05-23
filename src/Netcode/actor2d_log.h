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

} // namespace actor2d_log

#endif // ACTOR2D_LOG_H
