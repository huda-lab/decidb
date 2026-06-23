#include "catch.hpp"

#include "duckdb/decidb/decide_router.hpp"

using namespace duckdb;

namespace {

SolverResult MakeResult(SolverStatus status) {
	SolverResult result;
	result.status = status;
	return result;
}

} // namespace

// The router is a pure classifier: (status, mode) -> terminal. INF_OR_UNBD is
// already resolved to UNBOUNDED/INFEASIBLE upstream in SolveModel, so any residual
// INF_OR_UNBD reaching the router is the undecided case and routes to UNDIAGNOSED,
// alongside ITERATION_LIMIT and OTHER.
TEST_CASE("DeciDB query-diagnostics router", "[decidb][query_diagnostics][router]") {
	SECTION("auto mode routes each failed state to its terminal") {
		CHECK(RouteSolveResult(MakeResult(SolverStatus::OPTIMAL), "auto") == DiagnosisTerminal::SOLVED);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::UNBOUNDED), "auto") == DiagnosisTerminal::UNBOUNDED);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::INFEASIBLE), "auto") == DiagnosisTerminal::INFEASIBLE);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::TIME_LIMIT), "auto") == DiagnosisTerminal::TIME_LIMIT);
	}

	SECTION("off mode suppresses diagnosis: every failed state is UNDIAGNOSED") {
		CHECK(RouteSolveResult(MakeResult(SolverStatus::UNBOUNDED), "off") == DiagnosisTerminal::UNDIAGNOSED);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::INFEASIBLE), "off") == DiagnosisTerminal::UNDIAGNOSED);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::TIME_LIMIT), "off") == DiagnosisTerminal::UNDIAGNOSED);
	}

	SECTION("off mode still routes a successful solve to SOLVED") {
		CHECK(RouteSolveResult(MakeResult(SolverStatus::OPTIMAL), "off") == DiagnosisTerminal::SOLVED);
	}

	SECTION("statuses no engine covers are UNDIAGNOSED in both modes") {
		for (const char *mode : {"auto", "off"}) {
			CHECK(RouteSolveResult(MakeResult(SolverStatus::INF_OR_UNBD), mode) == DiagnosisTerminal::UNDIAGNOSED);
			CHECK(RouteSolveResult(MakeResult(SolverStatus::ITERATION_LIMIT), mode) == DiagnosisTerminal::UNDIAGNOSED);
			CHECK(RouteSolveResult(MakeResult(SolverStatus::OTHER), mode) == DiagnosisTerminal::UNDIAGNOSED);
		}
	}
}
