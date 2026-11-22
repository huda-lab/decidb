#include "catch.hpp"

#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/decidb/formulation/solver_input.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "test_helpers.hpp"

#include <utility>

using namespace duckdb;

namespace {

// Minimal SolverInput with a trivial objective on var 0 so SolverModel::Build
// is happy; provenance is independent of the objective. Bounds default (row-scoped vars).
// Under DIAGNOSE, which is when a row carries provenance beyond its clause.
SolverInput MakeBaseInput(idx_t num_rows, idx_t num_vars, bool diagnose = true) {
	SolverInput input;
	input.diagnose = diagnose;
	input.num_rows = num_rows;
	input.num_decide_vars = num_vars;
	input.variable_types.assign(num_vars, LogicalType::DOUBLE);
	input.objective_variable_indices = {0};
	input.objective_coefficients = {CoefficientColumn::MakeScalar(1.0, num_rows)};
	return input;
}

// SUM over rows of (x[v0] + x[v1] + ...) <= rhs. Aggregate, ungrouped by default. Every clause the model builder reads
// arrives numbered (the emitter numbers it), so the fixture is clause 0.
EvaluatedConstraint MakeAggregate(const duckdb::vector<idx_t> &vars, idx_t num_rows, double rhs) {
	EvaluatedConstraint ec;
	ec.variable_indices = vars;
	for (idx_t i = 0; i < vars.size(); i++) {
		ec.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));
	}
	ec.rhs_values = CoefficientColumn::MakeScalar(rhs, num_rows);
	ec.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
	ec.lhs_is_aggregate = true;
	ec.provenance.origin.repair_group_id = 0;
	ec.provenance.shape = ElasticShape::PER_ROW_DATA;
	return ec;
}

// Per-row: x[v] <= rhs for every row. Non-aggregate. Clause 0, as MakeAggregate.
EvaluatedConstraint MakePerRow(idx_t var, idx_t num_rows, double rhs) {
	EvaluatedConstraint ec;
	ec.variable_indices = {var};
	ec.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));
	ec.rhs_values = CoefficientColumn::MakeScalar(rhs, num_rows);
	ec.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
	ec.lhs_is_aggregate = false;
	ec.provenance.origin.repair_group_id = 0;
	ec.provenance.shape = ElasticShape::PER_ROW_DATA;
	return ec;
}

SolverModel BuildModel(SolverInput &input) {
	VarIndexer indexer = VarIndexer::Build(input);
	return SolverModel::Build(input, indexer);
}

} // namespace

TEST_CASE("DeciDB F2 constraint provenance", "[decidb][query_diagnostics][provenance]") {
	SECTION("aggregate ungrouped stamps clause id with no group") {
		SolverInput input = MakeBaseInput(2, 2);
		input.constraints.push_back(MakeAggregate({0, 1}, 2, 10.0));

		SolverModel model = BuildModel(input);

		REQUIRE(model.constraints.size() == 1);
		CHECK(model.constraints[0].provenance.origin.repair_group_id == 0);
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
			CHECK(c.provenance.origin.repair_group_id == 0);
			CHECK(c.provenance.kind == ConstraintKind::USER_PARAMETER);
		}
		// Groups emit in order g = 0, 1.
		CHECK(model.constraints[0].provenance.group_key == 0);
		CHECK(model.constraints[1].provenance.group_key == 1);
	}

	SECTION("per-row stamps the clause id on every emitted row") {
		SolverInput input = MakeBaseInput(3, 1);
		input.constraints.push_back(MakePerRow(0, 3, 5.0));
		// `x <= 5` on one decision folds into the column box unless the query is under DIAGNOSE; provenance is what
		// the diagnosis reads, and under it the rows are kept.

		SolverModel model = BuildModel(input);

		REQUIRE(model.constraints.size() == 3);
		for (auto &c : model.constraints) {
			CHECK(c.provenance.origin.repair_group_id == 0);
			CHECK(c.provenance.group_key == DConstants::INVALID_INDEX);
			CHECK(c.provenance.kind == ConstraintKind::USER_PARAMETER);
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
		// The structural global row comes first, the user clause after it.
		CHECK(model.constraints[0].provenance.kind == ConstraintKind::STRUCTURAL);
		CHECK(model.constraints[0].provenance.origin.repair_group_id == DConstants::INVALID_INDEX);
		CHECK(!IsRelaxableForElastic(model.constraints[0].provenance.kind));
		CHECK(model.constraints[1].provenance.kind == ConstraintKind::USER_PARAMETER);
	}

	SECTION("raw rows preserve source identity separately from repair grouping") {
		SolverInput input = MakeBaseInput(1, 1);
		SolverInput::RawConstraint raw;
		raw.indices = {0};
		raw.coefficients = {1.0};
		raw.sense = '<';
		raw.rhs = 5.0;
		raw.provenance.kind = ConstraintKind::USER_PARAMETER;
		raw.provenance.shape = ElasticShape::SHARED_SCALAR;
		raw.provenance.origin.source_clause_id = 7;
		raw.provenance.origin.repair_group_id = 11;
		input.global_constraints.push_back(std::move(raw));

		SolverModel model = BuildModel(input);

		REQUIRE(model.constraints.size() == 1);
		CHECK(model.constraints[0].provenance.origin.source_clause_id == 7);
		CHECK(model.constraints[0].provenance.origin.repair_group_id == 11);
		CHECK(model.constraints[0].provenance.shape == ElasticShape::SHARED_SCALAR);
	}

	SECTION("an ordinary query's rows name their clause and carry nothing else") {
		SolverInput input = MakeBaseInput(2, 2, false);
		auto aggregate = MakeAggregate({0, 1}, 2, 10.0);
		aggregate.provenance.origin.source_clause_id = 5;
		aggregate.provenance.origin.repair_group_id = 3;
		aggregate.comparison_type = ExpressionType::COMPARE_LESSTHAN; // a strict bound remembers the typed K
		input.constraints.push_back(std::move(aggregate));
		SolverInput::RawConstraint raw;
		raw.indices = {0};
		raw.coefficients = {1.0};
		raw.sense = '<';
		raw.rhs = 100.0;
		raw.provenance.origin.source_clause_id = 7;
		raw.provenance.origin.repair_group_id = 11;
		input.global_constraints.push_back(std::move(raw));
		GeneralConstraintSpec general;
		general.result_column = 1;
		general.argument_columns = {0};
		general.provenance.origin.source_clause_id = 9;
		general.provenance.origin.repair_group_id = 13;
		input.general_constraints.push_back(std::move(general));

		SolverModel model = BuildModel(input);

		REQUIRE(model.constraints.size() == 2);
		CHECK(model.constraints[0].provenance.origin.source_clause_id == 7);
		CHECK(model.constraints[1].provenance.origin.source_clause_id == 5);
		REQUIRE(model.general_constraints.size() == 1);
		CHECK(model.general_constraints[0].provenance.origin.source_clause_id == 9);
		for (auto *prov : {&model.constraints[0].provenance, &model.constraints[1].provenance,
		                   &model.general_constraints[0].provenance}) {
			CHECK(prov->origin.repair_group_id == DConstants::INVALID_INDEX);
			CHECK(prov->kind == ConstraintKind::USER_PARAMETER);
			CHECK(prov->shape == ElasticShape::UNSET);
			CHECK(!prov->strict);
			CHECK(prov->rhs_label.empty());
			CHECK(!prov->is_aggregate);
		}
	}

	SECTION("under DIAGNOSE a row carries its typed bound") {
		SolverInput input = MakeBaseInput(2, 2);
		auto aggregate = MakeAggregate({0, 1}, 2, 10.0);
		aggregate.comparison_type = ExpressionType::COMPARE_LESSTHAN;
		input.constraints.push_back(std::move(aggregate));
		GeneralConstraintSpec general;
		general.result_column = 1;
		general.argument_columns = {0};
		general.provenance.origin.repair_group_id = 13;
		input.general_constraints.push_back(std::move(general));

		SolverModel model = BuildModel(input);

		REQUIRE(model.constraints.size() == 1);
		auto &prov = model.constraints[0].provenance;
		CHECK(prov.strict);
		CHECK(prov.typed_k == 10.0);
		CHECK(prov.is_aggregate);
		REQUIRE(model.general_constraints.size() == 1);
		CHECK(model.general_constraints[0].provenance.kind == ConstraintKind::STRUCTURAL);
		CHECK(model.general_constraints[0].provenance.origin.repair_group_id == 13);
	}

	SECTION("a fixed aggregate LHS offset sums every row") {
		SolverInput valid = MakeBaseInput(2, 1);
		EvaluatedConstraint ec = MakeAggregate({0, DConstants::INVALID_INDEX}, 2, 10.0);
		ec.row_coefficients[1] = CoefficientColumn::MakeScalar(3.0, 2);
		valid.constraints.push_back(std::move(ec));

		SolverModel model = BuildModel(valid);
		REQUIRE(model.constraints.size() == 1);
		CHECK(model.constraints[0].rhs == 4.0);
	}
}

namespace {

LogicalDecide *FindProvenanceDecide(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DECIDE) {
		return &op.Cast<LogicalDecide>();
	}
	for (auto &child : op.children) {
		if (auto *found = FindProvenanceDecide(*child)) {
			return found;
		}
	}
	return nullptr;
}

} // namespace

TEST_CASE("A bound that is one value for the query is quoted by its value, not as an expression to edit",
          "[decidb][query_diagnostics][provenance]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
	// One clause per bound: a literal, a column, a subquery, and arithmetic over literals.
	auto plan = con.ExtractPlan("SELECT x FROM (FROM (VALUES (1, 2.0::DOUBLE)) s(id, w)"
	                            " DECIDE x(INT) SUCH THAT x <= 5 AND x <= w AND x <= (SELECT 5) AND x <= 2 + 3"
	                            " MAXIMIZE SUM(x)) q");
	auto *decide = FindProvenanceDecide(*plan);
	REQUIRE(decide != nullptr);
	auto &sources = decide->bound.constraint_sources;
	REQUIRE(sources.size() == 4);
	// A data bound is an expression the user can offset; every other bound is one number.
	CHECK(sources[0].rhs_kind == ConstraintSourceRhsKind::NUMERIC_FALLBACK);
	CHECK(sources[1].rhs_kind == ConstraintSourceRhsKind::DATA_EXPRESSION);
	CHECK(sources[2].rhs_kind == ConstraintSourceRhsKind::NUMERIC_FALLBACK);
	CHECK(sources[3].rhs_kind == ConstraintSourceRhsKind::NUMERIC_FALLBACK);
}

namespace {

//! `<n> <= <n + 1>`, stamped with a source clause id (none when `source_id` is INVALID_INDEX).
BoundDecideClause TaggedRow(idx_t source_id, int64_t n) {
	BoundDecideClause row;
	row.body = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO,
	                                                make_uniq<BoundConstantExpression>(Value::BIGINT(n)),
	                                                make_uniq<BoundConstantExpression>(Value::BIGINT(n + 1)));
	row.origin = DecideRowOrigin::OfClause(source_id);
	return row;
}

//! A clause with no span, which a plan quotes by its canonical rendering.
ConstraintSourceInfo WrittenClause(const string &text) {
	ConstraintSourceInfo source;
	source.canonical_lhs = text;
	return source;
}

//! The layers of `rows`, for a registry of `sources`.
duckdb::vector<DecideClauseLayers> LayersOf(duckdb::vector<BoundDecideClause> rows,
                                            const duckdb::vector<ConstraintSourceInfo> &sources,
                                            duckdb::vector<string> &unattributed) {
	DecideSourceText text;
	duckdb::vector<KeyedScopeInfo> keyed_scopes;
	duckdb::vector<DecideKey> keys;
	DecideSourceNames names {text, keyed_scopes, keys};
	duckdb::vector<string> objective_rows;
	return CollectDecideClauseLayers(rows, names, sources, unattributed, objective_rows, {}, {}, string());
}

} // namespace

TEST_CASE("A plan lists a clause by its source id, wherever the walk meets its rows", "[decidb][provenance][explain]") {
	SECTION("a row that no clause claims is listed apart, not under a clause") {
		duckdb::vector<BoundDecideClause> rows;
		rows.push_back(TaggedRow(0, 1));
		rows.push_back(TaggedRow(DConstants::INVALID_INDEX, 2));
		rows.push_back(TaggedRow(7, 3)); // an id the registry does not hold
		duckdb::vector<string> unattributed;
		auto layers = LayersOf(std::move(rows), {WrittenClause("clause 0")}, unattributed);
		REQUIRE(layers.size() == 1);
		CHECK(layers[0].written == "clause 0");
		REQUIRE(unattributed.size() == 2);
		CHECK(unattributed[0] == "2 <= 3");
		CHECK(unattributed[1] == "3 <= 4");
		auto text = RenderDecideClauseLayers(layers, unattributed);
		CHECK(text.find("added by formulation:") != string::npos);
	}
	SECTION("a clause no row mentions still prints, after the ones it does") {
		duckdb::vector<BoundDecideClause> rows;
		rows.push_back(TaggedRow(1, 1));
		duckdb::vector<string> unattributed;
		auto layers = LayersOf(std::move(rows), {WrittenClause("clause 0"), WrittenClause("clause 1")}, unattributed);
		REQUIRE(layers.size() == 2);
		CHECK(layers[0].written == "clause 1");
		CHECK(layers[1].written == "clause 0");
		CHECK(unattributed.empty());
	}
	SECTION("a BETWEEN prints its lower half first even when the walk meets the upper half first") {
		ConstraintSourceInfo lower = WrittenClause("x");
		lower.canonical_cmp = ">=";
		lower.canonical_rhs = "1";
		lower.between_partner = 1;
		ConstraintSourceInfo upper = lower;
		upper.canonical_cmp = "<=";
		upper.canonical_rhs = "3";
		upper.between_partner = 0;
		upper.between_upper = true;
		duckdb::vector<BoundDecideClause> rows;
		rows.push_back(TaggedRow(1, 3));
		rows.push_back(TaggedRow(0, 1));
		duckdb::vector<string> unattributed;
		auto layers = LayersOf(std::move(rows), {lower, upper}, unattributed);
		REQUIRE(layers.size() == 1);
		REQUIRE(layers[0].canonical.size() == 2);
		CHECK(layers[0].canonical[0] == "x >= 1");
		CHECK(layers[0].canonical[1] == "x <= 3");
	}
}
