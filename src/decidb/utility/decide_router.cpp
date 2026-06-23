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

DiagnosisTerminal RouteSolveResult(const SolverResult &result, const string &mode) {
	switch (result.status) {
	case SolverStatus::OPTIMAL:
		return DiagnosisTerminal::SOLVED;
	case SolverStatus::UNBOUNDED:
		return DiagnosisApplies(mode, result.status) ? DiagnosisTerminal::UNBOUNDED
		                                             : DiagnosisTerminal::UNDIAGNOSED;
	case SolverStatus::INFEASIBLE:
		return DiagnosisApplies(mode, result.status) ? DiagnosisTerminal::INFEASIBLE
		                                             : DiagnosisTerminal::UNDIAGNOSED;
	case SolverStatus::TIME_LIMIT:
		return DiagnosisApplies(mode, result.status) ? DiagnosisTerminal::TIME_LIMIT
		                                             : DiagnosisTerminal::UNDIAGNOSED;
	default:
		// Residual INF_OR_UNBD (SolveModel's probe could not decide), ITERATION_LIMIT,
		// OTHER: no engine covers these — fall to the static solver error.
		return DiagnosisTerminal::UNDIAGNOSED;
	}
}

} // namespace duckdb
