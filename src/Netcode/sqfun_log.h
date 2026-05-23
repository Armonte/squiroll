#pragma once

#ifndef SQFUN_LOG_H
#define SQFUN_LOG_H 1

// Diagnostic: SafetyHookInline on Sqrat::Function::Execute_1_param
// (0xB860). Per-call dump of the function/env SQObject so the rollback
// asymmetry inside Update_mask1's Squirrel callback can be attributed to
// a specific script function. See sqfun_log.cpp.

namespace sqfun_log {

void install();

} // namespace sqfun_log

#endif // SQFUN_LOG_H
