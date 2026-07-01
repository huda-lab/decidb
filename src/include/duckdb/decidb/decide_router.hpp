//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/decide_router.hpp
//
// The query-diagnostics router: the single post-solve dispatch spine. It is a
// pure classifier — given the solve result and the diagnose_decide mode, it names
// the terminal the operator should route to. It owns no engine invocation and no
// execution/operator types, so the decision tree is unit-testable in isolation.
// See context/descriptions/08_query_diagnostics/router/.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/solver_result.hpp"

namespace duckdb {

//! A leaf of the router's dispatch tree — the terminal a solve routes to.
//! INFEASIBLE and TIME_LIMIT are classified distinctly; INFEASIBLE is wired to the
//! elastic engine, while TIME_LIMIT still shares the static-error path until R6.
enum class DiagnosisTerminal {
	//! OPTIMAL: store the solution (the success path).
	SOLVED,
	//! Failed unbounded, diagnosis requested: run the unbounded engine.
	UNBOUNDED,
	//! Failed infeasible, diagnosis requested: run the elastic engine.
	INFEASIBLE,
	//! Hit the time limit, diagnosis requested: slow terminal (R6; static error until then).
	TIME_LIMIT,
	//! No diagnosis: mode is off, or a status no engine covers (ITERATION_LIMIT,
	//! OTHER). Falls to the static solver error.
	UNDIAGNOSED
};

//! Classify a solve result into its terminal under the given diagnose_decide
//! `mode`. Pure: depends only on `result.status` and the mode policy
//! (`DiagnosisApplies`), plus the residual INF_OR_UNBD ray sub-signal. Existing
//! solver/facade status probes still run first; if INF_OR_UNBD survives, a found
//! ray routes to UNBOUNDED and no ray routes to INFEASIBLE.
DiagnosisTerminal RouteSolveResult(const SolverResult &result, const string &mode);

} // namespace duckdb
