//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/solver/solver_result.hpp
//
// Structured result returned by the solver backends and the SolveModel facade.
// Carries the solver status (so callers can branch on the outcome instead of
// catching an exception) alongside the solution. This is the F1 foundation of
// the query-diagnostics area (context/descriptions/07_query_diagnostics/).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

#include <cmath>
#include <limits>

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
	//! A feasible solution is available but the solver could not prove it optimal
	//! (Gurobi GRB_SUBOPTIMAL — a numerically hard barrier on a QCP/SOC model that
	//! stops before satisfying optimality tolerances). Carries `solution` /
	//! `objective_value` / `has_solution` like the TIME_LIMIT incumbent; the operator
	//! delivers the rows with a "not proven best" caveat instead of discarding them.
	SUBOPTIMAL,
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
	//! `status == OPTIMAL` (the proven optimum), at `status == TIME_LIMIT` when the
	//! backend found a feasible incumbent (`has_solution`), and at
	//! `status == SUBOPTIMAL` (feasible but unproven); empty otherwise. Callers must
	//! therefore branch on `status` / `has_solution`, not on emptiness alone, to know
	//! whether a value is proven optimal.
	vector<double> solution;
	//! Objective value of `solution`, in the model's own sense (no sign flip).
	//! The proven optimum at `OPTIMAL`; the best-so-far incumbent objective at
	//! `TIME_LIMIT` / `SUBOPTIMAL` (only meaningful when `has_solution`); left 0.0 otherwise.
	//! The infeasible engine's stage-2 re-solve reads this as the achievable objective.
	//! Stage-1 repair budgets are read back from the solved repair variables.
	double objective_value = 0.0;
	//! Unbounded ray (filled by U2; empty for F1).
	vector<double> ray;
	//! True when an internal diagnostic helper solve (INF_OR_UNBD probe or portable
	//! ray fallback) hit its diagnostic budget before producing a definitive result.
	//! The primary status is still reported normally; this bit lets the operator say
	//! diagnosis ran out of time instead of falling back to a misleading static error.
	bool diagnostic_timed_out = false;
	//! Backend-native status code, surfaced in the OTHER catch-all message.
	int raw_status = 0;
	//! TIME_LIMIT / SUBOPTIMAL: true when the backend found a feasible incumbent
	//! (TIME_LIMIT by the time limit; SUBOPTIMAL when the solver stopped without a
	//! proof of optimality). Gurobi `SolCount > 0` / HiGHS `primal_solution_status ==
	//! feasible`. Gates the incumbent reads (`solution`, `objective_value`,
	//! `gap`) — with no incumbent those attributes return solver sentinels
	//! (`-1e100` / `inf` / `nan`), so they are read only when this is true.
	bool has_solution = false;
	//! TIME_LIMIT only: the solver's best proven bound on the objective (Gurobi
	//! `ObjBound` / HiGHS `mip_dual_bound`); bounds how far the incumbent can
	//! still improve. NaN when no bound exists — LP/QP timeouts (neither backend
	//! proves a bound there), a failed attribute read, or a solver infinity
	//! sentinel. Report writers must skip the bound when `!std::isfinite`.
	double best_bound = std::numeric_limits<double>::quiet_NaN();
	//! TIME_LIMIT only: relative optimality gap between `objective_value` and
	//! `best_bound` (Gurobi `MIPGap` / HiGHS `mip_gap`), as a fraction. Only
	//! meaningful when `has_solution`; NaN whenever `best_bound` is unavailable
	//! (same cases as above). Report writers must skip it when `!std::isfinite`.
	double gap = std::numeric_limits<double>::quiet_NaN();
	//! TIME_LIMIT only: true when the stop was a user Ctrl-C (`GRB_INTERRUPTED`),
	//! not the wall-clock limit. Drives report wording ("you stopped it" vs "hit
	//! the … time limit"), not routing — an interrupt reuses the whole TIME_LIMIT
	//! terminal. Gurobi-only; HiGHS has no thread-safe terminate, so it never sets it.
	bool user_interrupted = false;
	//! Benchmark instrumentation: the number of constraint rows `SolverModel::Build`
	//! actually handed the backend, linear + quadratic. This is the post-expansion
	//! count — a per-row clause spec becomes one row per data row and a PER spec one row
	//! per group — so it is the only figure comparable across queries. Set even when the
	//! solve fails; 0 when Build proved infeasibility and no model was ever produced.
	idx_t model_constraint_rows = 0;
};

//! Throws the default user-facing DECIDE error for a non-optimal `result`.
//! Single home for the message text that both backends used to duplicate. The
//! operator calls this on the UNDIAGNOSED terminal only — either the statement was
//! not prefixed with DIAGNOSE, or the status has no engine (ITERATION_LIMIT / OTHER).
//! A diagnosed failure returns findings instead of throwing.
[[noreturn]] void ThrowDecideSolveError(const SolverResult &result);

} // namespace duckdb
