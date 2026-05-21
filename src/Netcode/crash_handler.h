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

} // namespace crash_handler

#endif // CRASH_HANDLER_H
