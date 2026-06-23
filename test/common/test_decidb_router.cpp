#include "catch.hpp"

#include "duckdb/decidb/decide_router.hpp"

#include <utility>

using namespace duckdb;

namespace {

SolverResult MakeResult(SolverStatus status, duckdb::vector<double> ray = duckdb::vector<double>()) {
	SolverResult result;
	result.status = status;
	result.ray = std::move(ray);
	return result;
}

} // namespace

// The router is a pure classifier: (status + mode + residual INF_OR_UNBD ray
// signal) -> terminal. Existing solver/facade probes still run before this point;
// if INF_OR_UNBD survives, the router sends a found ray to the unbounded terminal
// and no ray to the infeasible terminal.
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
		CHECK(RouteSolveResult(MakeResult(SolverStatus::INF_OR_UNBD, {1.0}), "off") ==
		      DiagnosisTerminal::UNDIAGNOSED);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::INF_OR_UNBD), "off") ==
		      DiagnosisTerminal::UNDIAGNOSED);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::TIME_LIMIT), "off") == DiagnosisTerminal::UNDIAGNOSED);
	}

	SECTION("off mode still routes a successful solve to SOLVED") {
		CHECK(RouteSolveResult(MakeResult(SolverStatus::OPTIMAL), "off") == DiagnosisTerminal::SOLVED);
	}

	SECTION("residual INF_OR_UNBD routes by ray signal in auto mode") {
		CHECK(RouteSolveResult(MakeResult(SolverStatus::INF_OR_UNBD, {1.0}), "auto") ==
		      DiagnosisTerminal::UNBOUNDED);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::INF_OR_UNBD), "auto") ==
		      DiagnosisTerminal::INFEASIBLE);
	}

	SECTION("statuses no engine covers are UNDIAGNOSED in both modes") {
		for (const char *mode : {"auto", "off"}) {
			CHECK(RouteSolveResult(MakeResult(SolverStatus::ITERATION_LIMIT), mode) == DiagnosisTerminal::UNDIAGNOSED);
			CHECK(RouteSolveResult(MakeResult(SolverStatus::OTHER), mode) == DiagnosisTerminal::UNDIAGNOSED);
		}
	}
}
