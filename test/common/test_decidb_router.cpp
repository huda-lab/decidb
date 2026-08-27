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

// The router is a pure classifier: (status + DIAGNOSE prefix + residual INF_OR_UNBD
// ray signal) -> terminal. Existing solver/facade probes still run before this point;
// if INF_OR_UNBD survives, the router sends a found ray to the unbounded terminal
// and no ray to the infeasible terminal.
TEST_CASE("DeciDB query-diagnostics router", "[decidb][query_diagnostics][router]") {
	SECTION("under DIAGNOSE each failed state routes to its terminal") {
		CHECK(RouteSolveResult(MakeResult(SolverStatus::OPTIMAL), true) == DiagnosisTerminal::SOLVED);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::UNBOUNDED), true) == DiagnosisTerminal::UNBOUNDED);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::INFEASIBLE), true) == DiagnosisTerminal::INFEASIBLE);
	}

	SECTION("without the prefix every failed state is UNDIAGNOSED") {
		CHECK(RouteSolveResult(MakeResult(SolverStatus::UNBOUNDED), false) == DiagnosisTerminal::UNDIAGNOSED);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::INFEASIBLE), false) == DiagnosisTerminal::UNDIAGNOSED);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::INF_OR_UNBD, {1.0}), false) ==
		      DiagnosisTerminal::UNDIAGNOSED);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::INF_OR_UNBD), false) ==
		      DiagnosisTerminal::UNDIAGNOSED);
	}

	SECTION("a successful solve routes to SOLVED with or without the prefix") {
		CHECK(RouteSolveResult(MakeResult(SolverStatus::OPTIMAL), false) == DiagnosisTerminal::SOLVED);
	}

	SECTION("residual INF_OR_UNBD routes by ray signal under DIAGNOSE") {
		CHECK(RouteSolveResult(MakeResult(SolverStatus::INF_OR_UNBD, {1.0}), true) ==
		      DiagnosisTerminal::UNBOUNDED);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::INF_OR_UNBD), true) ==
		      DiagnosisTerminal::INFEASIBLE);
	}

	SECTION("a time limit is never a diagnosis terminal") {
		// A slow solve is ordinary execution behaviour — the operator handles it before
		// the router runs, prefix or no prefix.
		CHECK(RouteSolveResult(MakeResult(SolverStatus::TIME_LIMIT), true) == DiagnosisTerminal::UNDIAGNOSED);
		CHECK(RouteSolveResult(MakeResult(SolverStatus::TIME_LIMIT), false) == DiagnosisTerminal::UNDIAGNOSED);
	}

	SECTION("statuses no engine covers are UNDIAGNOSED either way") {
		for (bool armed : {true, false}) {
			CHECK(RouteSolveResult(MakeResult(SolverStatus::ITERATION_LIMIT), armed) == DiagnosisTerminal::UNDIAGNOSED);
			CHECK(RouteSolveResult(MakeResult(SolverStatus::OTHER), armed) == DiagnosisTerminal::UNDIAGNOSED);
		}
	}
}
