#include "catch.hpp"

#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/decidb/solver_input.hpp"

#include <utility>

using namespace duckdb;

namespace {

// Minimal SolverInput with a trivial MAXIMIZE objective on var 0 so SolverModel::Build
// is happy; provenance is independent of the objective. Bounds default (row-scoped vars).
SolverInput MakeBaseInput(idx_t num_rows, idx_t num_vars) {
	SolverInput input;
	input.num_rows = num_rows;
	input.num_decide_vars = num_vars;
	input.variable_types.assign(num_vars, LogicalType::DOUBLE);
	input.objective_variable_indices = {0};
	input.objective_coefficients = {CoefficientColumn::MakeScalar(1.0, num_rows)};
	input.sense = DecideSense::MAXIMIZE;
	return input;
}

// SUM over rows of (x[v0] + x[v1] + ...) <= rhs. Aggregate, ungrouped by default.
EvaluatedConstraint MakeAggregate(const duckdb::vector<idx_t> &vars, idx_t num_rows, double rhs) {
	EvaluatedConstraint ec;
	ec.variable_indices = vars;
	for (idx_t i = 0; i < vars.size(); i++) {
		ec.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));
	}
	ec.rhs_values = CoefficientColumn::MakeScalar(rhs, num_rows);
	ec.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
	ec.lhs_is_aggregate = true;
	return ec;
}

// Per-row: x[v] <= rhs for every row. Non-aggregate.
EvaluatedConstraint MakePerRow(idx_t var, idx_t num_rows, double rhs) {
	EvaluatedConstraint ec;
	ec.variable_indices = {var};
	ec.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));
	ec.rhs_values = CoefficientColumn::MakeScalar(rhs, num_rows);
	ec.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
	ec.lhs_is_aggregate = false;
	return ec;
}

SolverModel BuildModel(SolverInput &input) {
	VarIndexer indexer = VarIndexer::BuildRef(input);
	return SolverModel::Build(input, indexer);
}

} // namespace

TEST_CASE("DeciDB F2 constraint provenance", "[decidb][query_diagnostics][provenance]") {
	SECTION("aggregate ungrouped stamps clause id with no group") {
		SolverInput input = MakeBaseInput(2, 2);
		input.constraints.push_back(MakeAggregate({0, 1}, 2, 10.0));

		SolverModel model = BuildModel(input);

		REQUIRE(model.constraints.size() == 1);
		CHECK(model.constraints[0].provenance.repair_group_id == 0);
		CHECK(model.constraints[0].provenance.group_key == DConstants::INVALID_INDEX);
		CHECK(model.constraints[0].provenance.kind == ConstraintKind::USER_PARAMETER);
		CHECK(IsRelaxableForElastic(model.constraints[0].provenance.kind));
	}

	SECTION("aggregate + PER stamps one row per group with distinct group keys") {
		SolverInput input = MakeBaseInput(4, 2);
		EvaluatedConstraint ec = MakeAggregate({0, 1}, 4, 10.0);
		ec.row_group_ids = {0, 0, 1, 1};
		ec.num_groups = 2;
		input.constraints.push_back(std::move(ec));

		SolverModel model = BuildModel(input);

		REQUIRE(model.constraints.size() == 2);
		for (auto &c : model.constraints) {
			CHECK(c.provenance.repair_group_id == 0);
			CHECK(c.provenance.kind == ConstraintKind::USER_PARAMETER);
		}
		// Groups emit in order g = 0, 1.
		CHECK(model.constraints[0].provenance.group_key == 0);
		CHECK(model.constraints[1].provenance.group_key == 1);
	}

	SECTION("per-row stamps the clause id on every emitted row") {
		SolverInput input = MakeBaseInput(3, 1);
		input.constraints.push_back(MakePerRow(0, 3, 5.0));

		SolverModel model = BuildModel(input);

		REQUIRE(model.constraints.size() == 3);
		for (auto &c : model.constraints) {
			CHECK(c.provenance.repair_group_id == 0);
			CHECK(c.provenance.group_key == DConstants::INVALID_INDEX);
			CHECK(c.provenance.kind == ConstraintKind::USER_PARAMETER);
		}
	}

	SECTION("distinct clauses receive distinct clause ids") {
		SolverInput input = MakeBaseInput(3, 2);
		input.constraints.push_back(MakeAggregate({0, 1}, 3, 10.0)); // clause 0
		input.constraints.push_back(MakePerRow(0, 3, 5.0));          // clause 1

		SolverModel model = BuildModel(input);

		// 1 aggregate row + 3 per-row rows.
		REQUIRE(model.constraints.size() == 4);
		CHECK(model.constraints[0].provenance.repair_group_id == 0);
		for (idx_t i = 1; i < 4; i++) {
			CHECK(model.constraints[i].provenance.repair_group_id == 1);
		}
	}

	SECTION("raw global constraints are tagged STRUCTURAL with no clause") {
		SolverInput input = MakeBaseInput(2, 2);
		input.constraints.push_back(MakeAggregate({0, 1}, 2, 10.0)); // clause 0
		SolverInput::RawConstraint raw;
		raw.indices = {0};
		raw.coefficients = {1.0};
		raw.sense = '<';
		raw.rhs = 100.0;
		input.global_constraints.push_back(std::move(raw));

		SolverModel model = BuildModel(input);

		REQUIRE(model.constraints.size() == 2);
		// User clause first, structural global row appended last.
		CHECK(model.constraints[0].provenance.kind == ConstraintKind::USER_PARAMETER);
		CHECK(model.constraints[1].provenance.kind == ConstraintKind::STRUCTURAL);
		CHECK(model.constraints[1].provenance.repair_group_id == DConstants::INVALID_INDEX);
		CHECK(!IsRelaxableForElastic(model.constraints[1].provenance.kind));
	}

	SECTION("raw rows preserve source identity separately from repair grouping") {
		SolverInput input = MakeBaseInput(1, 1);
		SolverInput::RawConstraint raw;
		raw.indices = {0};
		raw.coefficients = {1.0};
		raw.sense = '<';
		raw.rhs = 5.0;
		raw.kind = ConstraintKind::USER_PARAMETER;
		raw.shape = ElasticShape::SHARED_SCALAR;
		raw.source_clause_id = 7;
		raw.repair_group_id = 11;
		input.global_constraints.push_back(std::move(raw));

		SolverModel model = BuildModel(input);

		REQUIRE(model.constraints.size() == 1);
		CHECK(model.constraints[0].provenance.source_clause_id == 7);
		CHECK(model.constraints[0].provenance.repair_group_id == 11);
		CHECK(model.constraints[0].provenance.shape == ElasticShape::SHARED_SCALAR);
	}

	SECTION("fixed aggregate LHS offsets require an explicit SUM owner") {
		SolverInput valid = MakeBaseInput(2, 1);
		EvaluatedConstraint ec = MakeAggregate({0, DConstants::INVALID_INDEX}, 2, 10.0);
		ec.row_coefficients[1] = CoefficientColumn::MakeScalar(3.0, 2);
		ec.linear_term_reductions = {LinearTermReduction::SUM, LinearTermReduction::SUM};
		valid.constraints.push_back(std::move(ec));

		SolverModel model = BuildModel(valid);
		REQUIRE(model.constraints.size() == 1);
		CHECK(model.constraints[0].rhs == 4.0);

		SolverInput invalid = MakeBaseInput(2, 1);
		EvaluatedConstraint malformed = MakeAggregate({0, DConstants::INVALID_INDEX}, 2, 10.0);
		malformed.row_coefficients[1] = CoefficientColumn::MakeScalar(3.0, 2);
		malformed.linear_term_reductions = {LinearTermReduction::SUM, LinearTermReduction::NONE};
		invalid.constraints.push_back(std::move(malformed));
		CHECK_THROWS_AS(BuildModel(invalid), InternalException);
	}

}
