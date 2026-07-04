//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/solver_config.hpp
//
// Solver-agnostic run-time configuration shared by every DECIDE solve, applied
// identically across both backends (Gurobi / HiGHS). Centralizing it here is the
// single source of truth: a user-set override resolves once and governs both
// solvers, so the two backends can never drift apart on the same knob.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdlib>
#include <string>

namespace duckdb {

//! Default wall-clock cap (seconds) on a single DECIDE solve, applied to BOTH
//! backends (Gurobi `TimeLimit`, HiGHS `time_limit`). Keeps a hard MILP/MIQP/QCQP
//! from hanging the session indefinitely; on timeout the backend returns its best
//! feasible incumbent, mapped to SolverStatus::TIME_LIMIT.
constexpr double DECIDE_DEFAULT_TIME_LIMIT_SECONDS = 300.0;

//! Diagnostic follow-up solves are bounded separately from the primary solve. A
//! failed primary solve should not be followed by several full-length internal
//! re-solves before the user sees a result.
constexpr double DECIDE_DIAGNOSTIC_TIME_LIMIT_SECONDS = 60.0;

//! Resolve the per-solve time limit shared by both solver backends. Returns the
//! default unless the user overrides it via the global `DECIDB_TIME_LIMIT`
//! environment variable (seconds, double); non-positive or unparseable values are
//! ignored and the default is kept. This is the one place the limit is read, so it
//! is solver-agnostic by construction: changing the env var changes the cap on
//! Gurobi and HiGHS identically.
inline double ResolveDecideTimeLimit() {
	double time_limit = DECIDE_DEFAULT_TIME_LIMIT_SECONDS;
	if (const char *env_limit = std::getenv("DECIDB_TIME_LIMIT")) {
		try {
			double parsed = std::stod(env_limit);
			if (parsed > 0.0) {
				time_limit = parsed;
			}
		} catch (...) {
			// Ignore unparseable values; keep the default.
		}
	}
	return time_limit;
}

//! Resolve the budget for one diagnostic follow-up solve. It is capped at the
//! smaller of the primary solve's configured budget and the diagnostic cap.
inline double ResolveDecideDiagnosticTimeLimit(double primary_time_limit = -1.0) {
	double limit = primary_time_limit > 0.0 ? primary_time_limit : ResolveDecideTimeLimit();
	return limit < DECIDE_DIAGNOSTIC_TIME_LIMIT_SECONDS ? limit : DECIDE_DIAGNOSTIC_TIME_LIMIT_SECONDS;
}

} // namespace duckdb
