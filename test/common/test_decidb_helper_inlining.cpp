#include "catch.hpp"
#include "decidb_clause_records.hpp"

#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/decidb/formulation/solver_input.hpp"

#include <algorithm>
#include <functional>

using namespace duckdb;

// What stage 06 receives of the reducer helpers (pipeline/06_model_formulation/done.md, "A helper written where it is
// read"), on SolverInputs built by hand: four rows, `x` per row (decision 0), a helper `h` per group of rows {0, 1} and
// {2, 3} (decision 1, keyed). Stage 08 writes a helper's members into its readers; stage 06 lays out no column for an
// instance written in, and defines the instances that keep one.

namespace {

constexpr idx_t ROWS = 4;

SolverInput MakeInput(idx_t num_vars) {
	SolverInput input;
	input.num_rows = ROWS;
	input.num_decide_vars = num_vars;
	input.variable_types.assign(num_vars, LogicalType::DOUBLE);
	input.lower_bounds.assign(num_vars, 0.0);
	input.upper_bounds.assign(num_vars, 1e30);
	input.variable_scopes.assign(num_vars, DecideVarScopeInfo::Row());
	input.variable_scopes[1] = DecideVarScopeInfo::Keyed(0);
	KeyedMapping groups;
	groups.num_classes = 2;
	groups.row_to_class = {0, 0, 1, 1};
	input.keyed_mappings.push_back(std::move(groups));
	if (num_vars > 2) {
		input.variable_scopes[2] = DecideVarScopeInfo::Global();
	}
	return input;
}

//! `h`: instance 0 is `x0 + 2 x1 + 0.5`, instance 1 is `x2 + x3`.
ReducerValueLinkSpec SumOfX() {
	ReducerValueLinkSpec link;
	link.value_var = 1;
	link.reducer = DecideReducerKind::SUM;
	link.label = "sum(x) by (g)";
	auto &members = link.members;
	members.AddGroup(0);
	members.AddMember(0.5);
	members.AddTerm(0, 0, 1.0);
	members.AddMember(0.0);
	members.AddTerm(0, 1, 2.0);
	members.AddGroup(1);
	members.AddMember(0.0);
	members.AddTerm(0, 2, 1.0);
	members.AddMember(0.0);
	members.AddTerm(0, 3, 1.0);
	return link;
}

//! `x - h <= 10` on every row.
EvaluatedConstraint XLessH() {
	EvaluatedConstraint ec;
	ec.variable_indices = {0, 1};
	for (idx_t row = 0; row < ROWS; row++) {
		// h's instance is the row's group.
		AddRecord(ec, {RecordTerm {0, row, 1.0}, RecordTerm {1, row / 2, -1.0}}, 10.0);
	}
	ec.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
	return ec;
}

//! A row as `(column, coefficient)` pairs in column order.
duckdb::vector<std::pair<int, double>> Terms(const duckdb::vector<int> &indices,
                                             const duckdb::vector<double> &coefficients) {
	duckdb::vector<std::pair<int, double>> terms;
	for (idx_t i = 0; i < indices.size(); i++) {
		terms.emplace_back(indices[i], coefficients[i]);
	}
	std::sort(terms.begin(), terms.end());
	return terms;
}

} // namespace

TEST_CASE("Stage 06 lays out and defines the helper instances that keep a column", "[decidb][formulation][inlining]") {
	const duckdb::vector<string> names = {"x", "h", "m"};
	const NativeConstructPolicy lowering {false, false};

	SECTION("a sum with a column has one per group and a definition per group") {
		SolverInput input = MakeInput(2);
		input.reducer_value_links.push_back(SumOfX());
		input.constraints.push_back(XLessH());

		VarIndexer indexer = VarIndexer::Build(input);
		CHECK(indexer.total_vars == ROWS + 2);
		LinearizeReducerValueLinks(input, indexer, names, lowering);
		CHECK(input.global_constraints.size() == 2); // h_g - members = constants
	}

	SECTION("a keyed helper written where it is read has no column") {
		// Stage 08 wrote `x - h <= 10` as `x_r - (its group's members) <= 10`, the 0.5 in the record's constant.
		SolverInput input = MakeInput(2);
		input.written_in.resize(2);
		input.written_in[1] = {true, true};
		EvaluatedConstraint ec;
		ec.variable_indices = {0, 1};
		AddRecord(ec, {RecordTerm {0, 0, 1.0}, RecordTerm {0, 0, -1.0}, RecordTerm {0, 1, -2.0}}, 10.0);
		ec.records.constants[0] = -0.5;
		ec.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
		input.constraints.push_back(std::move(ec));

		VarIndexer indexer = VarIndexer::Build(input);
		CHECK(indexer.total_vars == ROWS); // x alone
		CHECK_FALSE(indexer.HasColumn(1, 0));
		CHECK(indexer.ColumnOrNone(1, 3) == DConstants::INVALID_INDEX);

		SolverModel model = SolverModel::Build(input, indexer);
		REQUIRE(model.constraints.size() == 1);
		auto &row = model.constraints[0];
		CHECK(Terms(row.indices, row.coefficients) == duckdb::vector<std::pair<int, double>> {{1, -2.0}});
		CHECK(row.rhs == Approx(10.5));
	}

	SECTION("a query-wide helper written where it is read has no column") {
		SolverInput input = MakeInput(3);
		input.written_in.resize(3);
		input.written_in[2] = {true};
		VarIndexer indexer = VarIndexer::Build(input);
		CHECK(indexer.total_vars == ROWS + 2); // x and h
		CHECK(indexer.NumInstances(2) == 0);
		CHECK_FALSE(indexer.HasColumn(2, 0));
	}

	SECTION("a group written where it is read leaves the helper's other groups their columns") {
		SolverInput input = MakeInput(2);
		input.written_in.resize(2);
		input.written_in[1] = {true, false};
		VarIndexer indexer = VarIndexer::Build(input);
		CHECK(indexer.total_vars == ROWS + 1);
		CHECK_FALSE(indexer.HasColumn(1, 0));
		CHECK(indexer.Get(1, 2) == ROWS);
	}

	SECTION("the objective's coefficient of an instance is its column's, and a helper written in has none") {
		// MINIMIZE x + (2 * h per row), h written in: stage 08 summed x's instances with h's members', twice each.
		SolverInput input = MakeInput(2);
		input.written_in.resize(2);
		input.written_in[1] = {true, true};
		input.objective_coefficients = {{5.0, 9.0, 5.0, 5.0}, {}};
		input.objective_constant = 2.0; // 2 * 0.5, read by rows 0 and 1
		VarIndexer indexer = VarIndexer::Build(input);
		SolverModel model = SolverModel::Build(input, indexer);
		CHECK(model.obj_constant == Approx(2.0));
		CHECK(model.obj_coeffs == duckdb::vector<double> {5.0, 9.0, 5.0, 5.0});
	}

	SECTION("a maximum's group of one member that keeps its column is an equality") {
		// h = max per group: {x0} and {x2, x3}, read where only its column can stand.
		SolverInput input = MakeInput(2);
		ReducerValueLinkSpec link;
		link.value_var = 1;
		link.reducer = DecideReducerKind::MAX;
		link.need_envelope = true;
		link.members.AddGroup(0);
		link.members.AddMember(0.0);
		link.members.AddTerm(0, 0, 1.0);
		link.members.AddGroup(1);
		link.members.AddMember(0.0);
		link.members.AddTerm(0, 2, 1.0);
		link.members.AddMember(0.0);
		link.members.AddTerm(0, 3, 1.0);
		input.reducer_value_links.push_back(std::move(link));

		VarIndexer indexer = VarIndexer::Build(input);
		LinearizeReducerValueLinks(input, indexer, names, lowering);
		// h_0 - x0 = 0, then h_1's envelope: h_1 - x2 >= 0, h_1 - x3 >= 0.
		REQUIRE(input.global_constraints.size() == 3);
		auto &equality = input.global_constraints[0];
		CHECK(equality.sense == '=');
		CHECK(Terms(equality.indices, equality.coefficients) ==
		      duckdb::vector<std::pair<int, double>> {{0, -1.0}, {static_cast<int>(indexer.Get(1, 0)), 1.0}});
		CHECK(input.global_constraints[1].sense == '>');
		CHECK(input.global_constraints[2].sense == '>');
	}
}
