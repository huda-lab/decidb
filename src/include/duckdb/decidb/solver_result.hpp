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

//! Result of SolveModel / a backend Solve(). `status` + `solution` are the core
//! fields; `ray` is reserved for unbounded-ray extraction (U2); the trailing
//! `has_solution` / `best_bound` / `gap` fields carry the timeout incumbent (S1).
struct SolverResult {
	//! Normalized terminal status of the solve.
	SolverStatus status = SolverStatus::OTHER;
	//! Solution vector (size = num_rows * num_decide_vars). Populated at
	//! `status == OPTIMAL` (the proven optimum) and at `status == TIME_LIMIT`
	//! when the backend found a feasible incumbent (`has_solution`); empty
	//! otherwise. Callers must therefore branch on `status` / `has_solution`,
	//! not on emptiness alone, to know whether a value is proven optimal.
	vector<double> solution;
	//! Objective value of `solution`, in the model's own sense (no sign flip).
	//! The proven optimum at `OPTIMAL`; the best-so-far incumbent objective at
	//! `TIME_LIMIT` (only meaningful when `has_solution`); left 0.0 otherwise.
	//! The infeasible engine's stage-2 re-solve reads this as the achievable objective
	//! (and stage-1 as the total loosening S*).
	double objective_value = 0.0;
	//! Unbounded ray (filled by U2; empty for F1).
	vector<double> ray;
	//! Backend-native status code, surfaced in the OTHER catch-all message.
	int raw_status = 0;
	//! TIME_LIMIT only: true when the backend found a feasible incumbent by the
	//! time limit (Gurobi `SolCount > 0` / HiGHS `primal_solution_status ==
	//! feasible`). Gates the incumbent reads (`solution`, `objective_value`,
	//! `gap`) — with no incumbent those attributes return solver sentinels
	//! (`-1e100` / `inf` / `nan`), so they are read only when this is true.
	bool has_solution = false;
	//! TIME_LIMIT only: the solver's best proven bound on the objective (Gurobi
	//! `ObjBound` / HiGHS `mip_dual_bound`). Always meaningful at the time limit
	//! regardless of `has_solution`; bounds how far the incumbent can still improve.
	double best_bound = 0.0;
	//! TIME_LIMIT only: relative optimality gap between `objective_value` and
	//! `best_bound` (Gurobi `MIPGap` / HiGHS `mip_gap`), as a fraction. Only
	//! meaningful when `has_solution`.
	double gap = 0.0;
};

//! Throws the default user-facing DECIDE error for a non-optimal `result`.
//! Single home for the message text that both backends used to duplicate; the
//! operator calls this when no diagnosis pragma is active (manual-first). The
//! F4 pragma will later gate this call.
[[noreturn]] void ThrowDecideSolveError(const SolverResult &result);

} // namespace duckdb
