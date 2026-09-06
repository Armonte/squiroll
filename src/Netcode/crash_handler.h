#pragma once

#ifndef CRASH_HANDLER_H
#define CRASH_HANDLER_H 1

// Vectored exception handler. On a fatal fault (access violation, illegal
// instruction, ...) it writes a synchronous crash report to aocf_crash.log
// next to the exe — faulting instruction, access target, registers, and an
// EBP-chain stack walk, every address resolved to module+RVA so it drops
// straight into the IDA db. The async logger can't be trusted once the
// process is faulting; this writes with raw WriteFile.

namespace crash_handler {

// Install the VEH. Call once, as early as possible in common_init.
void install();

// While enabled, the VEH also logs first-chance C++ exceptions (0xE06D7363)
// — normally ignored. Used to catch an uncaught C++/Sqrat throw inside a
// rollback re-sim advance (which otherwise terminates with no crash log).
void watch_cxx(bool on);

// DIAGNOSTIC: hardware data-write watchpoint. watchpoint_arm(addr) traps
// every 4-byte write to `addr` and logs the writing instruction's EIP +
// the new value; watchpoint_disarm() clears it. MUST be called on the
// thread to be watched (debug registers are per-thread) — i.e. the
// simulation thread. Used to find which code writes a rollback-divergent
// field.
void watchpoint_arm(void* addr);
void watchpoint_disarm();

// HANG DIAGNOSIS: suspend + ebp-walk every other thread of the process into
// the log. Called by the gekko_bridge hang watchdog when the forward frame
// stops advancing, so a silent stall becomes a named loop.
void dump_all_thread_stacks(const char* why);

// Register a th155 worker thread (call from the _beginthreadex hook). The
// hardware watchpoint then arms DR0 on every registered thread, not just
// the simulation thread — needed to catch a non-sim-thread writer.
void register_thread(uint32_t tid);

void start_ondemand_dump_thread();
} // namespace crash_handler

#endif // CRASH_HANDLER_H
