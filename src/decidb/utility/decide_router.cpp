//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/decidb/utility/decide_router.cpp
//
// Implementation of the query-diagnostics router classifier. See the header for
// the dispatch-tree contract.
//
//===----------------------------------------------------------------------===//

#include "duckdb/decidb/decide_router.hpp"

#include "duckdb/decidb/decide_diagnostic.hpp"

namespace duckdb {

DiagnosisTerminal RouteSolveResult(const SolverResult &result, bool armed) {
	switch (result.status) {
	case SolverStatus::OPTIMAL:
		return DiagnosisTerminal::SOLVED;
	case SolverStatus::SUBOPTIMAL:
		// A feasible-but-unproven incumbent (Gurobi stopped without proving optimality
		// on a numerically hard QCP). It is a usable answer, so deliver it as a success;
		// the operator adds the "not proven best" caveat. The backend guarantees a
		// solution is present whenever it reports SUBOPTIMAL.
		return DiagnosisTerminal::SOLVED;
	case SolverStatus::UNBOUNDED:
		return DiagnosisApplies(armed, result.status) ? DiagnosisTerminal::UNBOUNDED
		                                             : DiagnosisTerminal::UNDIAGNOSED;
	case SolverStatus::INFEASIBLE:
		return DiagnosisApplies(armed, result.status) ? DiagnosisTerminal::INFEASIBLE
		                                             : DiagnosisTerminal::UNDIAGNOSED;
	case SolverStatus::TIME_LIMIT:
		// Not a diagnosis state. A slow solve is handled on the ordinary execution path
		// — checkpoint report, then continue or take the incumbent — with or without the
		// DIAGNOSE prefix, and the operator deals with it before the router ever runs.
		// Reaching here means the timeout was not handled, so fall to the plain error.
		return DiagnosisTerminal::UNDIAGNOSED;
	case SolverStatus::INF_OR_UNBD:
		if (!DiagnosisApplies(armed, result.status)) {
			return DiagnosisTerminal::UNDIAGNOSED;
		}
		return result.ray.empty() ? DiagnosisTerminal::INFEASIBLE : DiagnosisTerminal::UNBOUNDED;
	default:
		// ITERATION_LIMIT / OTHER: no engine covers these — fall to the static
		// solver error.
		return DiagnosisTerminal::UNDIAGNOSED;
	}
}

} // namespace duckdb
