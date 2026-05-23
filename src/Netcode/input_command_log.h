#pragma once

#ifndef INPUT_COMMAND_LOG_H
#define INPUT_COMMAND_LOG_H 1

// Diagnostic: SafetyHookInline on Manbow::InputCommand::Update (0x74390).
// Dumps the input source's class vtable pointer per call inside a frame
// window so the un-synced input source (the per-frame divergence root
// surfaced by battle_pools' panopticon) can be named.

namespace input_command_log {

void install();

} // namespace input_command_log

#endif // INPUT_COMMAND_LOG_H
