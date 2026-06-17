//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/solver_result.hpp
//
// Structured result returned by the solver backends and the SolveModel facade.
// Carries the solver status (so callers can branch on the outcome instead of
// catching an exception) alongside the solution. This is the F1 foundation of
// the query-diagnostics area (context/descriptions/08_query_diagnostics/).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

//! Outcome of a solve, normalized across backends (Gurobi / HiGHS).
//! INF_OR_UNBD is the ambiguous "infeasible or unbounded" terminal status:
//! Gurobi's GRB_INF_OR_UNBD (after the DualReductions=0 re-solve fails to
//! disambiguate) and HiGHS's kUnboundedOrInfeasible (status 9) both map here.
enum class SolverStatus {
	OPTIMAL,
	INFEASIBLE,
	UNBOUNDED,
	INF_OR_UNBD,
	TIME_LIMIT,
	ITERATION_LIMIT,
	OTHER
};

//! Result of SolveModel / a backend Solve(). For F1 only `status` and
//! `solution` are populated; `ray` is reserved for unbounded-ray extraction
//! (U2). Timeout incumbent / objective / best-bound / gap fields are
//! intentionally deferred to the slow branch (S2).
struct SolverResult {
	//! Normalized terminal status of the solve.
	SolverStatus status = SolverStatus::OTHER;
	//! Solution vector (size = num_rows * num_decide_vars). Present only when
	//! `status == OPTIMAL`; empty otherwise.
	vector<double> solution;
	//! Unbounded ray (filled by U2; empty for F1).
	vector<double> ray;
	//! Backend-native status code, surfaced in the OTHER catch-all message.
	int raw_status = 0;
};

//! Throws the default user-facing DECIDE error for a non-optimal `result`.
//! Single home for the message text that both backends used to duplicate; the
//! operator calls this when no diagnosis pragma is active (manual-first). The
//! F4 pragma will later gate this call.
[[noreturn]] void ThrowDecideSolveError(const SolverResult &result);

} // namespace duckdb
