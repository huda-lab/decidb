//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/diagnostic_constants.hpp
//
// Shared numeric thresholds for the diagnostic re-solve / unbounded-ray path.
// These were previously redefined per-file; they must agree for the box-LP
// "free suspect filter" invariant to hold (the set of bounds opened in the
// fallback LP and the set of ray components deemed non-zero must be computed
// against consistent thresholds across the builder, solver, and engine).
//
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

//! A finite bound at or above this magnitude is treated as "effectively
//! infinite" (i.e. the variable is unbounded in that direction). Used to decide
//! which upper bounds to open in the box-LP ray-fallback model.
constexpr double EFFECTIVE_INFINITY = 1e20;

//! A ray / objective-improvement component whose absolute value is at or below
//! this epsilon is treated as zero. Used both to confirm the fallback LP found a
//! genuine improving ray and to filter which variables actually escape.
constexpr double DIAGNOSTIC_RAY_EPSILON = 1e-8;

//! Elastic-program objective weight for a data-RHS (PER_ROW_DATA) slack, relative
//! to an editable-knob slack (weight 1). A data-RHS constraint (`x <= col`) cannot
//! be edited by the user — loosening it is not an actionable fix, only a conflict —
//! so penalizing its slack steers the solver to loosen editable constraints first
//! and report a clean single-knob edit; data conflicts surface only when editable
//! loosening genuinely cannot restore feasibility. (A coarse stand-in for the I3
//! lexicographic tie-break.)
constexpr double DIAGNOSTIC_DATA_SLACK_WEIGHT = 1e3;

} // namespace duckdb
