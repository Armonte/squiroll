#pragma once

#ifndef CL_ITER_GUARD_H
#define CL_ITER_GUARD_H 1

// Defensive hook on concurrent_list_iter_step. Short-circuits the walk if
// the iter position field holds an arena base (a known corruption pattern
// we haven't yet attributed). See cl_iter_guard.cpp.

namespace cl_iter_guard {

void install();

// Dynamic-VB ring high-water report (see report_dynvb_peak in the .cpp). Called
// from the frame loop roughly once a second; resets the peak each time.
void report_dynvb_peak();

} // namespace cl_iter_guard

#endif // CL_ITER_GUARD_H
