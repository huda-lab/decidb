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
	input.sense = DecideSense::MAXIMIZE;
	return input;
}

//! `h`: group 0 is `x0 + 2 x1 + 0.5`, group 1 is `x2 + x3`.
ReducerValueLinkSpec SumOfX() {
	ReducerValueLinkSpec link;
	link.value_var = 1;
	link.agg_name = "sum";
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

//! `max(h at row 0, h at row 2)` (decision 2), closing on one side.
ReducerValueLinkSpec ClosingMaxOfH(duckdb::vector<idx_t> rows) {
	ReducerValueLinkSpec outer;
	outer.value_var = 2;
	outer.agg_name = "max";
	outer.label = "max(sum(x) by (g))";
	outer.need_closing = true;
	outer.members.AddGroup(0);
	for (auto row : rows) {
		outer.members.AddMember(0.0);
		outer.members.AddTerm(1, row, 1.0);
	}
	return outer;
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

TEST_CASE("DeciDB helper inlining", "[decidb][formulation][inlining]") {
	const duckdb::vector<string> names = {"x", "h", "m"};
	const NativeConstructPolicy lowering {false, false};

	SECTION("a sum read by two rows of two terms each is written into them") {
		SolverInput input = MakeInput(2);
		input.reducer_value_links.push_back(SumOfX());
		input.constraints.push_back(XLessH());

		CHECK(InlineReducerValueLinks(input, {}));
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
		// MAXIMIZE sum(2 * h) per row: each instance read by its group's rows, folded into
		// one objective -- 2 * (x0 + 2 x1 + 0.5) twice for group 0, 2 * (x2 + x3) twice.
		SolverInput input = MakeInput(2);
		input.reducer_value_links.push_back(SumOfX());
		input.objective_variable_indices = {1};
		input.objective_coefficients.push_back(CoefficientColumn::MakeScalar(2.0, ROWS));

		InlineReducerValueLinks(input, {});
		REQUIRE(input.variable_scopes[1].scope == DecideVarScope::INLINED);
		VarIndexer indexer = VarIndexer::Build(input);
		LinearizeReducerValueLinks(input, indexer, names, lowering);
		SolverModel model = SolverModel::Build(input, indexer);
		CHECK(model.obj_constant == Approx(2.0)); // 2 * 0.5, read by rows 0 and 1
		CHECK(model.obj_coeffs == duckdb::vector<double> {4.0, 8.0, 4.0, 4.0});
	}

	SECTION("a sum a reader holds keeps its column and definition") {
		SolverInput input = MakeInput(2);
		input.reducer_value_links.push_back(SumOfX());
		input.constraints.push_back(XLessH());

		CHECK_FALSE(InlineReducerValueLinks(input, {1}));
		CHECK(input.variable_scopes[1].scope == DecideVarScope::KEYED);
		VarIndexer indexer = VarIndexer::Build(input);
		CHECK(indexer.total_vars == ROWS + 2);
		LinearizeReducerValueLinks(input, indexer, names, lowering);
		CHECK(input.global_constraints.size() == 2); // h_g - members = constants
	}

	SECTION("every reader of a column holds it") {
		// h, unread, is written into nothing; each reader below reads its column instead (PlaceHelpers).
		auto row_over = [](idx_t var) {
			EvaluatedConstraint ec;
			ec.variable_indices = {var};
			ec.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, ROWS));
			ec.rhs_values = CoefficientColumn::MakeScalar(10.0, ROWS);
			ec.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
			return ec;
		};
		struct Reader {
			const char *name;
			idx_t num_vars;
			std::function<void(SolverInput &)> add;
		};
		duckdb::vector<Reader> readers = {
		    {"a frame read", 2,
		     [](SolverInput &input) {
			     SolverInput::FrameRead read;
			     read.base = 1;
			     read.rows = {1, 2, 3, 3};
			     input.frame_reads.push_back(std::move(read));
		     }},
		    {"a bilinear link", 2,
		     [](SolverInput &input) {
			     input.bilinear_links.push_back(BilinearLinkSpec {0, 1, 0});
		     }},
		    {"a row of a <> clause", 2,
		     [&](SolverInput &input) {
			     auto ec = row_over(1);
			     ec.ne_clause_idx = 0;
			     input.constraints.push_back(std::move(ec));
		     }},
		    {"a bound stage 08 may fold into a box", 2,
		     [&](SolverInput &input) {
			     auto ec = row_over(1);
			     ec.admitted_lower = CoefficientColumn::MakeScalar(0.0, ROWS);
			     ec.admitted_upper = CoefficientColumn::MakeScalar(10.0, ROWS);
			     input.constraints.push_back(std::move(ec));
		     }},
		    {"a product in a row", 2,
		     [&](SolverInput &input) {
			     auto ec = row_over(0);
			     ec.bilinear_terms.push_back({0, 1, CoefficientColumn::MakeScalar(1.0, ROWS)});
			     input.constraints.push_back(std::move(ec));
		     }},
		    {"a square in a row", 2,
		     [&](SolverInput &input) {
			     auto ec = row_over(0);
			     EvaluatedConstraint::QuadraticGroup group;
			     group.variable_indices = {1};
			     group.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, ROWS));
			     ec.quadratic_groups.push_back(std::move(group));
			     input.constraints.push_back(std::move(ec));
		     }},
		    {"a square in the objective", 2,
		     [](SolverInput &input) {
			     input.quadratic_inner_variable_indices = {1};
			     input.quadratic_inner_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, ROWS));
		     }},
		    {"a product in the objective", 2,
		     [](SolverInput &input) {
			     input.bilinear_objective_terms.push_back({0, 1, CoefficientColumn::MakeScalar(1.0, ROWS)});
		     }},
		    {"two links' members", 4,
		     [](SolverInput &input) {
			     input.variable_scopes[3] = DecideVarScopeInfo::Global();
			     input.reducer_value_links.push_back(ClosingMaxOfH({0, 2}));
			     auto other = ClosingMaxOfH({0, 2});
			     other.value_var = 3;
			     input.reducer_value_links.push_back(std::move(other));
		     }},
		    {"rows beside a link's members", 3,
		     [](SolverInput &input) {
			     input.constraints.push_back(XLessH());
			     input.reducer_value_links.push_back(ClosingMaxOfH({0, 2}));
		     }},
		    {"a maximum pinned on both sides", 3,
		     [](SolverInput &input) {
			     auto both_sides = ClosingMaxOfH({0, 2});
			     both_sides.need_envelope = true;
			     input.reducer_value_links.push_back(std::move(both_sides));
		     }},
		};
		for (auto &reader : readers) {
			INFO(reader.name);
			SolverInput input = MakeInput(reader.num_vars);
			input.reducer_value_links.push_back(SumOfX());
			reader.add(input);
			InlineReducerValueLinks(input, {});
			CHECK(input.variable_scopes[1].scope == DecideVarScope::KEYED);
		}
		// Read by nothing, it is written into nothing.
		SolverInput unread = MakeInput(2);
		unread.reducer_value_links.push_back(SumOfX());
		CHECK(InlineReducerValueLinks(unread, {}));
		CHECK(unread.variable_scopes[1].scope == DecideVarScope::INLINED);
	}

	SECTION("a sum whose readers would grow the model keeps its column") {
		// One instance of three terms read by three rows: 9 nonzeros inlined, 3 + 1 + 3 defined.
		SolverInput input = MakeInput(2);
		input.variable_scopes[1] = DecideVarScopeInfo::Global();
		ReducerValueLinkSpec link;
		link.value_var = 1;
		link.agg_name = "sum";
		link.members.AddGroup(0);
		for (idx_t r = 0; r < 3; r++) {
			link.members.AddMember(0.0);
			link.members.AddTerm(0, r, 1.0);
		}
		input.reducer_value_links.push_back(std::move(link));
		auto ec = XLessH();
		ec.row_group_ids = {0, 1, 2, DConstants::INVALID_INDEX};
		ec.num_groups = 3;
		input.constraints.push_back(std::move(ec));

		CHECK_FALSE(InlineReducerValueLinks(input, {}));
		CHECK(input.variable_scopes[1].scope == DecideVarScope::GLOBAL);
		CHECK_FALSE(input.reducer_value_links[0].inlined);
	}

	SECTION("a sum read by a maximum's members is written into them") {
		// m = max(h at row 0, 1 + 3 * h at row 2), pushed down: its envelope alone.
		SolverInput input = MakeInput(3);
		input.reducer_value_links.push_back(SumOfX());
		ReducerValueLinkSpec outer;
		outer.value_var = 2;
		outer.agg_name = "max";
		outer.need_envelope = true;
		outer.members.AddGroup(0);
		outer.members.AddMember(0.0);
		outer.members.AddTerm(1, 0, 1.0);
		outer.members.AddMember(1.0);
		outer.members.AddTerm(1, 2, 3.0);
		input.reducer_value_links.push_back(std::move(outer));

		InlineReducerValueLinks(input, {});
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

	SECTION("a maximum's closing side keeps a sum two of its members read") {
		// Closing natively, a member that is an expression costs a column and a row of its
		// own: written into two members, h_0 would cost two where its column cost one.
		SolverInput input = MakeInput(3);
		input.reducer_value_links.push_back(SumOfX());
		input.reducer_value_links.push_back(ClosingMaxOfH({0, 1}));
		InlineReducerValueLinks(input, {});
		CHECK(input.reducer_value_links.size() == 2);
		CHECK(input.variable_scopes[1].scope == DecideVarScope::KEYED);

		// Each instance read by one member: written in.
		SolverInput once = MakeInput(3);
		once.reducer_value_links.push_back(SumOfX());
		once.reducer_value_links.push_back(ClosingMaxOfH({0, 2}));
		InlineReducerValueLinks(once, {});
		CHECK(once.reducer_value_links.size() == 1);
	}

	SECTION("a maximum's group of one member is an equality") {
		// h = max per group: {x0} and {x2, x3}, read where only its column can stand.
		SolverInput input = MakeInput(2);
		ReducerValueLinkSpec link;
		link.value_var = 1;
		link.agg_name = "max";
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

		InlineReducerValueLinks(input, {1});
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
