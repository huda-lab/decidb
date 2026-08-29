#include "catch.hpp"

#include "duckdb/decidb/solver/probe_models.hpp"
#include "duckdb/decidb/solver/ilp_solver.hpp"

#include <cmath>
#include <utility>

using namespace duckdb;

namespace {

constexpr double DEFAULT_INFINITY = 1e30;
constexpr double RAY_EPSILON = 1e-8;

SolverModel MakeLinearModel(const duckdb::vector<double> &objective, bool maximize = true) {
	SolverModel model;
	model.num_vars = objective.size();
	model.col_lower.assign(model.num_vars, 0.0);
	model.col_upper.assign(model.num_vars, DEFAULT_INFINITY);
	model.is_integer.assign(model.num_vars, false);
	model.is_binary.assign(model.num_vars, false);
	model.obj_coeffs = objective;
	model.maximize = maximize;
	return model;
}

double ObjectiveValue(const SolverModel &model, const duckdb::vector<double> &solution) {
	double value = 0.0;
	for (idx_t col = 0; col < model.obj_coeffs.size(); col++) {
		value += model.obj_coeffs[col] * solution[col];
	}
	return value;
}

duckdb::vector<double> SolveFallbackRayWithHighs(const SolverModel &model) {
	SolverModel ray_model;
	REQUIRE(BuildUnboundedRayFallbackModel(model, ray_model));

	SolverResult result = SolvePreparedModel(ray_model, SolverRegistry::Find("highs"));
	REQUIRE(result.status == SolverStatus::OPTIMAL);
	REQUIRE(result.solution.size() == model.num_vars);

	double improvement = ObjectiveValue(ray_model, result.solution);
	if (improvement <= RAY_EPSILON) {
		return {};
	}
	return result.solution;
}

SolverInput MakeUnboundedTwoVariableInput() {
	SolverInput input;
	input.num_rows = 1;
	input.num_decide_vars = 2;
	input.variable_types = {LogicalType::DOUBLE, LogicalType::DOUBLE};
	input.objective_variable_indices = {0, 1};
	input.objective_coefficients = {
	    CoefficientColumn::MakeScalar(1.0, input.num_rows),
	    CoefficientColumn::MakeScalar(1.0, input.num_rows),
	};
	input.sense = DecideSense::MAXIMIZE;
	return input;
}

} // namespace

TEST_CASE("DeciDB query diagnostics unbounded ray fallback", "[decidb][query_diagnostics]") {
	SECTION("symmetric MAX LP returns full-support ray") {
		SolverModel model = MakeLinearModel({1.0, 1.0}, true);

		duckdb::vector<double> ray = SolveFallbackRayWithHighs(model);

		REQUIRE(ray.size() == 2);
		CHECK(std::fabs(ray[0] - 1.0) <= RAY_EPSILON);
		CHECK(std::fabs(ray[1] - 1.0) <= RAY_EPSILON);
	}

	SECTION("MINIMIZE with negative coefficients uses signed objective") {
		SolverModel model = MakeLinearModel({-2.0, -3.0}, false);

		duckdb::vector<double> ray = SolveFallbackRayWithHighs(model);

		REQUIRE(ray.size() == 2);
		CHECK(std::fabs(ray[0] - 1.0) <= RAY_EPSILON);
		CHECK(std::fabs(ray[1] - 1.0) <= RAY_EPSILON);
	}

	SECTION("finite original upper bounds force ray components to zero") {
		SolverModel model = MakeLinearModel({1.0, 1.0}, true);
		model.col_upper[0] = 5.0;

		duckdb::vector<double> ray = SolveFallbackRayWithHighs(model);

		REQUIRE(ray.size() == 2);
		CHECK(std::fabs(ray[0]) <= RAY_EPSILON);
		CHECK(std::fabs(ray[1] - 1.0) <= RAY_EPSILON);
	}

	SECTION("bounded feasible shape yields no improving ray") {
		SolverModel model = MakeLinearModel({1.0, 1.0}, true);
		ModelConstraint constraint;
		constraint.indices = {0, 1};
		constraint.coefficients = {1.0, 1.0};
		constraint.sense = '<';
		constraint.rhs = 10.0;
		model.constraints.push_back(std::move(constraint));

		duckdb::vector<double> ray = SolveFallbackRayWithHighs(model);

		CHECK(ray.empty());
	}

	SECTION("greater-equal row sense is preserved in homogenization") {
		// Objective on x0 only, with x1 >= x0 (-x0 + x1 >= 0). The '>=' row must
		// survive homogenization: pushing d0 to its box max forces d1 up to match.
		// A flipped sense ('<=') would leave d1 free to stay at 0, so asserting
		// d1 == 1 guards against the wrong-sign branch.
		SolverModel model = MakeLinearModel({1.0, 0.0}, true);
		ModelConstraint constraint;
		constraint.indices = {0, 1};
		constraint.coefficients = {-1.0, 1.0};
		constraint.sense = '>';
		constraint.rhs = 0.0;
		model.constraints.push_back(std::move(constraint));

		duckdb::vector<double> ray = SolveFallbackRayWithHighs(model);

		REQUIRE(ray.size() == 2);
		CHECK(std::fabs(ray[0] - 1.0) <= RAY_EPSILON);
		CHECK(std::fabs(ray[1] - 1.0) <= RAY_EPSILON);
	}

	SECTION("equality row couples ray components") {
		// Objective on x0 only, with x0 == x1. The '=' row ties d1 to d0, so the
		// ray names both even though only x0 carries objective weight.
		SolverModel model = MakeLinearModel({1.0, 0.0}, true);
		ModelConstraint constraint;
		constraint.indices = {0, 1};
		constraint.coefficients = {1.0, -1.0};
		constraint.sense = '=';
		constraint.rhs = 0.0;
		model.constraints.push_back(std::move(constraint));

		duckdb::vector<double> ray = SolveFallbackRayWithHighs(model);

		REQUIRE(ray.size() == 2);
		CHECK(std::fabs(ray[0] - 1.0) <= RAY_EPSILON);
		CHECK(std::fabs(ray[1] - 1.0) <= RAY_EPSILON);
	}

	SECTION("integer flags are relaxed in the ray LP") {
		SolverModel model = MakeLinearModel({1.0, 1.0}, true);
		model.is_integer = {true, true};

		SolverModel ray_model;
		REQUIRE(BuildUnboundedRayFallbackModel(model, ray_model));
		REQUIRE(ray_model.is_integer.size() == 2);
		CHECK(!ray_model.is_integer[0]);
		CHECK(!ray_model.is_integer[1]);

		duckdb::vector<double> ray = SolveFallbackRayWithHighs(model);
		REQUIRE(ray.size() == 2);
		CHECK(std::fabs(ray[0] - 1.0) <= RAY_EPSILON);
		CHECK(std::fabs(ray[1] - 1.0) <= RAY_EPSILON);
	}

	SECTION("SolveModel attaches rays only when requested") {
		SolverInput default_input = MakeUnboundedTwoVariableInput();
		VarIndexer default_indexer = VarIndexer::Build(default_input);

		SolverResult default_result =
		    SolveModel(default_input, default_indexer, SelectSolverBackend(), SolveModelOptions());
		CHECK(default_result.status == SolverStatus::UNBOUNDED);
		CHECK(default_result.ray.empty());

		SolverInput diagnostic_input = MakeUnboundedTwoVariableInput();
		VarIndexer diagnostic_indexer = VarIndexer::Build(diagnostic_input);
		SolveModelOptions options;
		options.extract_unbounded_ray = true;

		SolverResult diagnostic_result =
		    SolveModel(diagnostic_input, diagnostic_indexer, SelectSolverBackend(), options);
		CHECK(diagnostic_result.status == SolverStatus::UNBOUNDED);
		REQUIRE(diagnostic_result.ray.size() == 2);
		CHECK(std::fabs(diagnostic_result.ray[0] - 1.0) <= RAY_EPSILON);
		CHECK(std::fabs(diagnostic_result.ray[1] - 1.0) <= RAY_EPSILON);
	}
}
