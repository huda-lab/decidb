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

//! Elastic-program objective weight for a data-RHS (PER_ROW_DATA) slack. Editable
//! knobs sit at a scale-normalized base weight in (0, 1] (`ref / rms(Aᵢ)`, T1 — see
//! BuildElasticModel); this flat data weight sits an order of magnitude above them.
//! A data-RHS constraint (`x <= col`) cannot be edited by the user — loosening it is
//! not an actionable fix, only a conflict — so penalizing its slack steers the solver
//! to loosen editable constraints first and report a clean single-knob edit; data
//! conflicts surface only when editable loosening genuinely cannot restore
//! feasibility. (A coarse stand-in for the I3 lexicographic tie-break; only the
//! editable *within*-tier weights are normalized, cross-tier ordering is still T2.)
constexpr double DIAGNOSTIC_DATA_SLACK_WEIGHT = 1e3;

//! Elastic-program objective weight for a `<>` removal indicator (I4), relative to a
//! (scale-normalized, ≤ 1) editable-knob slack and a data slack (1e3). Dropping a `<>`
//! is the biggest hammer — it removes the clause rather than loosening it — so it sits
//! at the top of the weight ladder (editable ≤ 1 < data 1e3 < removal 1e6): the solver prefers
//! any loosening and removes a `<>` only when loosening genuinely cannot restore
//! feasibility. Like DIAGNOSTIC_DATA_SLACK_WEIGHT this is a coarse weighted stand-in;
//! it rides the same future lexicographic-tier conversion (see the infeasible
//! engine's "Notes to revisit").
constexpr double DIAGNOSTIC_REMOVAL_WEIGHT = 1e6;

} // namespace duckdb
