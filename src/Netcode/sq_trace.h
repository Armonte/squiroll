#pragma once

#ifndef SQ_TRACE_H
#define SQ_TRACE_H 1

// Diagnostic hook for the dual-rollback crash. The crash is a NULL-table
// SQTable__Get reached from th155's SQInstance member-get (sub_180AD0):
// it does SQTable__Get(instance->_class->_members, key) and _members is
// NULL. This hooks sub_180AD0 to log the offending instance, class and
// member name the instant that condition appears — and returns
// member-not-found instead of letting it crash, so a run keeps going and
// every occurrence is captured.

namespace sq_trace {
void install();
}

#endif // SQ_TRACE_H
