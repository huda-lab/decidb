//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/decide_router.hpp
//
// The query-diagnostics router: the single post-solve dispatch spine. It is a
// pure classifier — given the solve result and whether the statement carried the
// DIAGNOSE prefix, it names the terminal the operator should route to. It owns no
// engine invocation and no execution/operator types, so the decision tree is
// unit-testable in isolation.
// See context/descriptions/07_query_diagnostics/router/.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/solver_result.hpp"

namespace duckdb {

//! A leaf of the router's dispatch tree — the terminal a solve routes to.
enum class DiagnosisTerminal {
	//! OPTIMAL: store the solution (the success path).
	SOLVED,
	//! Failed unbounded under DIAGNOSE: run the unbounded engine.
	UNBOUNDED,
	//! Failed infeasible under DIAGNOSE: run the elastic engine.
	INFEASIBLE,
	//! No diagnosis: no DIAGNOSE prefix, or a status no engine covers (TIME_LIMIT,
	//! which the execution layer handles before the router runs; ITERATION_LIMIT;
	//! OTHER). Falls to the static solver error.
	UNDIAGNOSED
};

//! Classify a solve result into its terminal. `armed` is the statement's DIAGNOSE
//! prefix. Pure: depends only on `result.status` and that flag (`DiagnosisApplies`),
//! plus the residual INF_OR_UNBD ray sub-signal. Existing solver/facade status probes
//! still run first; if INF_OR_UNBD survives, a found ray routes to UNBOUNDED and no ray
//! routes to INFEASIBLE.
DiagnosisTerminal RouteSolveResult(const SolverResult &result, bool armed);

} // namespace duckdb
