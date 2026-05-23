#pragma once

#ifndef INPUT_GLOBAL_SYNC_H
#define INPUT_GLOBAL_SYNC_H 1

// FIX: capture-and-replay Manbow::InputMulti's polled input state so the
// per-frame InputCommand ring stays deterministic across rollback re-sims.
// See input_global_sync.cpp for the full diagnosis + strategy comment.

namespace input_global_sync {

void install();

} // namespace input_global_sync

#endif // INPUT_GLOBAL_SYNC_H
