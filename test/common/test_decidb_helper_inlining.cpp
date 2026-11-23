#include "catch.hpp"

#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/decidb/formulation/solver_input.hpp"

#include <algorithm>
#include <functional>

using namespace duckdb;

// Stage 06's helper inlining (D89, pipeline/06_model_formulation/done.md, "Inlining a helper"),
// on SolverInputs built by hand: four rows, `x` per row (decision 0), a SUM helper `h` per
// group of rows {0, 1} and {2, 3} (decision 1, keyed), and for the folding cases a
// query-wide MAX helper `m` over `h` (decision 2).

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

//! `h`: group 0 is `x0 + 2 x1 + 0.5`, group 1 is `x2 + x3`, in the form the plan gave it.
ReducerValueLinkSpec SumOfX(DecideHelperForm form) {
	ReducerValueLinkSpec link;
	link.form = form;
	link.value_var = 1;
	link.reducer = DecideReducerKind::SUM;
	link.label = "sum(x) by (g)";
	auto &members = link.members;
	members.AddGroup(0);
	members.AddMember(0.5);
	members.AddTerm(0, 0, 1.0);
	members.AddMember(0.0);
	members.AddTerm(0, 1, 2.0);
	members.AddGroup(2);
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
	ec.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, ROWS));
	ec.row_coefficients.push_back(CoefficientColumn::MakeScalar(-1.0, ROWS));
	ec.rhs_values = CoefficientColumn::MakeScalar(10.0, ROWS);
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

TEST_CASE("A helper is written where the plan's form says", "[decidb][formulation][inlining]") {
	const duckdb::vector<string> names = {"x", "h", "m"};
	const NativeConstructPolicy lowering {false, false};

	SECTION("a sum the plan writes in is written into the rows that read it") {
		SolverInput input = MakeInput(2);
		input.diagnose = true; // a row records what its lowering added to the bound only for a diagnosis
		input.reducer_value_links.push_back(SumOfX(DecideHelperForm::WRITTEN_IN));
		input.constraints.push_back(XLessH());

		CHECK(WriteHelpersIn(input));
		REQUIRE(input.variable_scopes[1].scope == DecideVarScope::INLINED);
		REQUIRE(input.reducer_value_links.size() == 1);
		CHECK(input.reducer_value_links[0].inlined);

		VarIndexer indexer = VarIndexer::Build(input);
		CHECK(indexer.total_vars == ROWS); // x alone: the helper owns no column
		CHECK(indexer.NumInstances(1) == 0);
		CHECK(indexer.InstanceOf(1, 3) == 1);
		CHECK_THROWS_AS(indexer.Get(1, 0), InternalException);

		LinearizeReducerValueLinks(input, indexer, names, lowering);
		CHECK(input.global_constraints.empty()); // no definition
		REQUIRE(input.inlined_helpers.size() == 1);

		SolverModel model = SolverModel::Build(input, indexer);
		// x_r - (its group's members) <= 10, the 0.5 moved to the bound: row 0 cancels x0.
		REQUIRE(model.constraints.size() == 4);
		auto &row0 = model.constraints[0];
		CHECK(Terms(row0.indices, row0.coefficients) == duckdb::vector<std::pair<int, double>> {{1, -2.0}});
		CHECK(row0.rhs == Approx(10.5));
		// The diagnosis quotes the bound as written: 10.
		CHECK(row0.rhs - row0.provenance.rhs_mechanism_offset == Approx(10.0));
		auto &row1 = model.constraints[1];
		CHECK(Terms(row1.indices, row1.coefficients) == duckdb::vector<std::pair<int, double>> {{0, -1.0}, {1, -1.0}});
		CHECK(row1.rhs == Approx(10.5));
		auto &row3 = model.constraints[3];
		CHECK(Terms(row3.indices, row3.coefficients) == duckdb::vector<std::pair<int, double>> {{2, -1.0}});
		CHECK(row3.rhs == Approx(10.0));
	}

	SECTION("a sum written into the objective leaves its constants as the offset") {
		// MINIMIZE sum(2 * h) per row: each instance read by its group's rows, folded into
		// one objective -- 2 * (x0 + 2 x1 + 0.5) twice for group 0, 2 * (x2 + x3) twice.
		SolverInput input = MakeInput(2);
		input.reducer_value_links.push_back(SumOfX(DecideHelperForm::WRITTEN_IN));
		input.objective_variable_indices = {1};
		input.objective_coefficients.push_back(CoefficientColumn::MakeScalar(2.0, ROWS));

		WriteHelpersIn(input);
		REQUIRE(input.variable_scopes[1].scope == DecideVarScope::INLINED);
		VarIndexer indexer = VarIndexer::Build(input);
		LinearizeReducerValueLinks(input, indexer, names, lowering);
		SolverModel model = SolverModel::Build(input, indexer);
		CHECK(model.obj_constant == Approx(2.0)); // 2 * 0.5, read by rows 0 and 1
		CHECK(model.obj_coeffs == duckdb::vector<double> {4.0, 8.0, 4.0, 4.0});
	}

	SECTION("a sum the plan keeps a column for keeps its column and definition") {
		SolverInput input = MakeInput(2);
		input.reducer_value_links.push_back(SumOfX(DecideHelperForm::VARIABLE));
		input.constraints.push_back(XLessH());

		CHECK_FALSE(WriteHelpersIn(input));
		CHECK(input.variable_scopes[1].scope == DecideVarScope::KEYED);
		VarIndexer indexer = VarIndexer::Build(input);
		CHECK(indexer.total_vars == ROWS + 2);
		LinearizeReducerValueLinks(input, indexer, names, lowering);
		CHECK(input.global_constraints.size() == 2); // h_g - members = constants
	}

	SECTION("a sum read by a maximum's members is written into them") {
		// m = max(h at row 0, 1 + 3 * h at row 2), pushed down: its envelope alone.
		SolverInput input = MakeInput(3);
		input.reducer_value_links.push_back(SumOfX(DecideHelperForm::WRITTEN_IN));
		ReducerValueLinkSpec outer;
		outer.value_var = 2;
		outer.reducer = DecideReducerKind::MAX;
		outer.need_envelope = true;
		outer.members.AddGroup(0);
		outer.members.AddMember(0.0);
		outer.members.AddTerm(1, 0, 1.0);
		outer.members.AddMember(1.0);
		outer.members.AddTerm(1, 2, 3.0);
		input.reducer_value_links.push_back(std::move(outer));

		WriteHelpersIn(input);
		REQUIRE(input.reducer_value_links.size() == 1);
		CHECK(input.variable_scopes[1].scope == DecideVarScope::INLINED);
		auto &members = input.reducer_value_links[0].members;
		REQUIRE(members.GroupCount() == 1);
		REQUIRE(members.constants.size() == 2);
		// x0 + 2 x1 + 0.5, then 1 + 3 * (x2 + x3).
		CHECK(members.constants[0] == Approx(0.5));
		CHECK(members.constants[1] == Approx(1.0));
		CHECK(members.member_begin == duckdb::vector<idx_t> {0, 2, 4});
		CHECK(members.rows == duckdb::vector<idx_t> {0, 1, 2, 3});
		CHECK(members.coefficients == duckdb::vector<double> {1.0, 2.0, 3.0, 3.0});
	}

	SECTION("a maximum's group of one member is an equality where the plan keeps its column") {
		// h = max per group: {x0} and {x2, x3}, read where only its column can stand.
		SolverInput input = MakeInput(2);
		ReducerValueLinkSpec link;
		link.value_var = 1;
		link.reducer = DecideReducerKind::MAX;
		link.need_envelope = true;
		link.members.AddGroup(0);
		link.members.AddMember(0.0);
		link.members.AddTerm(0, 0, 1.0);
		link.members.AddGroup(2);
		link.members.AddMember(0.0);
		link.members.AddTerm(0, 2, 1.0);
		link.members.AddMember(0.0);
		link.members.AddTerm(0, 3, 1.0);
		input.reducer_value_links.push_back(std::move(link));

		CHECK_FALSE(WriteHelpersIn(input));
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

	SECTION("a maximum's group of one member is that member where the plan lets its readers take it") {
		// h = max per group: {x0} and {x2, x3}; `x - h <= 10` on every row reads it.
		SolverInput input = MakeInput(2);
		ReducerValueLinkSpec link;
		link.form = DecideHelperForm::ONE_MEMBER_WRITTEN_IN;
		link.value_var = 1;
		link.reducer = DecideReducerKind::MAX;
		link.need_envelope = true;
		link.members.AddGroup(0);
		link.members.AddMember(0.0);
		link.members.AddTerm(0, 0, 1.0);
		link.members.AddGroup(2);
		link.members.AddMember(0.0);
		link.members.AddTerm(0, 2, 1.0);
		link.members.AddMember(0.0);
		link.members.AddTerm(0, 3, 1.0);
		input.reducer_value_links.push_back(std::move(link));
		input.constraints.push_back(XLessH());

		CHECK(WriteHelpersIn(input));
		VarIndexer indexer = VarIndexer::Build(input);
		// The group of x2 and x3 alone has a column.
		CHECK(indexer.total_vars == ROWS + 1);
		LinearizeReducerValueLinks(input, indexer, names, lowering);
		// h_1's envelope and no row for group 0.
		CHECK(input.global_constraints.size() == 2);
	}
}
