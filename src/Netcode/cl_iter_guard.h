#pragma once

#ifndef CL_ITER_GUARD_H
#define CL_ITER_GUARD_H 1

// Defensive hook on concurrent_list_iter_step. Short-circuits the walk if
// the iter position field holds an arena base (a known corruption pattern
// we haven't yet attributed). See cl_iter_guard.cpp.

namespace cl_iter_guard {

void install();

} // namespace cl_iter_guard

#endif // CL_ITER_GUARD_H
