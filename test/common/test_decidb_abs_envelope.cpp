#include "catch.hpp"

#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/decidb/formulation/solver_input.hpp"

using namespace duckdb;

// The contract between stage 05 and the ABS rows of stage 06 (pipeline/06_model_formulation): stage 05 records each ABS
// and the emitter evaluates its two envelope rows; stage 06 states every row of the ABS from them. An envelope row
// holds no aggregate, because stage 05 gives every reducer a function reads a helper decision, so a row that kept one
// is refused where the rows are taken. SolverInputs are built by hand: `x` per row (decision 0) and the auxiliary `a`
// (decision 1).

namespace {

constexpr idx_t ABS_ROWS = 3;

//! `a - x >= 0` (C1) or `a + x >= 0` (C2), each tagged as the envelope row of auxiliary 1 over `x`.
EvaluatedConstraint EnvelopeRow(bool positive, bool aggregate) {
	EvaluatedConstraint ec;
	ec.variable_indices = {1, 0};
	ec.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, ABS_ROWS));
	ec.row_coefficients.push_back(CoefficientColumn::MakeScalar(positive ? -1.0 : 1.0, ABS_ROWS));
	ec.rhs_values = CoefficientColumn::MakeScalar(0.0, ABS_ROWS);
	ec.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	ec.abs_aux_idx = 1;
	ec.lhs_is_aggregate = aggregate;
	return ec;
}

//! One exact ABS over `x`, whose box is [-3, `x_upper`].
SolverInput AbsInput(bool aggregate, double x_upper = 2.0) {
	SolverInput input;
	input.num_rows = ABS_ROWS;
	input.num_decide_vars = 2;
	input.variable_types.assign(2, LogicalType::DOUBLE);
	input.lower_bounds = {-3.0, 0.0};
	input.upper_bounds = {x_upper, 1e30};
	input.variable_scopes.assign(2, DecideVarScopeInfo::Row());
	AbsLinkSpec link;
	link.aux_idx = 1;
	link.exact = true;
	link.operand = EnvelopeRow(true, aggregate);
	link.negated = EnvelopeRow(false, aggregate);
	input.abs_links.push_back(std::move(link));
	return input;
}

idx_t BinaryColumns(const SolverInput &input) {
	idx_t binaries = 0;
	for (auto &type : input.global_variable_types) {
		binaries += type == LogicalType::BOOLEAN;
	}
	return binaries;
}

} // namespace

TEST_CASE("DeciDB ABS rows are stated from the envelope rows", "[decidb][formulation][abs]") {
	duckdb::vector<string> names {"x", "a"};

	SECTION("a per-row envelope pair becomes two rows of the model") {
		SolverInput input = AbsInput(false);
		EmitAbsEnvelopes(input);
		REQUIRE(input.constraints.size() == 2);
		// `a >= x` and `a >= -x`; the link keeps `a >= x`, which the rows of an exact ABS are read from.
		CHECK(input.constraints[0].row_coefficients[1].Get(0) == -1.0);
		CHECK(input.constraints[1].row_coefficients[1].Get(0) == 1.0);
		CHECK(input.abs_links[0].operand.row_coefficients[1].Get(0) == -1.0);
	}

	SECTION("an envelope row that holds an aggregate is an internal error") {
		SolverInput input = AbsInput(true);
		CHECK_THROWS_AS(EmitAbsEnvelopes(input), InternalException);
		CHECK(input.constraints.empty());
	}

	SECTION("an exact ABS with a derivable reach is stated natively where the backend declares ABS") {
		SolverInput input = AbsInput(false);
		EmitAbsEnvelopes(input);
		auto indexer = VarIndexer::Build(input);
		DeriveAbsAuxiliaryBounds(input, indexer, names, /*refuse_when_unbounded=*/false);
		CHECK(input.abs_links[0].inner_lo == -3.0); // inner = x, over its box [-3, 2]
		CHECK(input.abs_links[0].inner_hi == 2.0);
		EmitAbsConstraints(input, indexer, NativeConstructPolicy {true, false, false});
		CHECK(input.general_constraints.size() == ABS_ROWS);
		CHECK(input.global_constraints.empty());
		CHECK(BinaryColumns(input) == 0);
	}

	SECTION("an exact ABS expands into a sign indicator and two Big-M rows where the backend declares none") {
		SolverInput input = AbsInput(false);
		EmitAbsEnvelopes(input);
		auto indexer = VarIndexer::Build(input);
		DeriveAbsAuxiliaryBounds(input, indexer, names, /*refuse_when_unbounded=*/false);
		EmitAbsConstraints(input, indexer, NativeConstructPolicy {false, false, false});
		CHECK(input.global_constraints.size() == 2 * ABS_ROWS);
		CHECK(BinaryColumns(input) == ABS_ROWS);
		CHECK(input.general_constraints.empty());
	}

	SECTION("a construct whose lowering is measured faster expands where its Big-M exists, and not where it does not") {
		SolverInput input = AbsInput(false);
		EmitAbsEnvelopes(input);
		auto indexer = VarIndexer::Build(input);
		DeriveAbsAuxiliaryBounds(input, indexer, names, /*refuse_when_unbounded=*/false);
		EmitAbsConstraints(input, indexer, NativeConstructPolicy {true, false, true});
		CHECK(input.global_constraints.size() == 2 * ABS_ROWS);
		CHECK(input.general_constraints.empty());

		SolverInput open = AbsInput(false, 1e30);
		EmitAbsEnvelopes(open);
		auto open_indexer = VarIndexer::Build(open);
		DeriveAbsAuxiliaryBounds(open, open_indexer, names, /*refuse_when_unbounded=*/false);
		EmitAbsConstraints(open, open_indexer, NativeConstructPolicy {true, false, true});
		CHECK(open.general_constraints.size() == ABS_ROWS);
	}

	SECTION(
	    "a backend that declares ABS states it natively where the reach has no end, and is refused where it does not") {
		SolverInput input = AbsInput(false, 1e30);
		EmitAbsEnvelopes(input);
		auto indexer = VarIndexer::Build(input);
		DeriveAbsAuxiliaryBounds(input, indexer, names, /*refuse_when_unbounded=*/false);
		CHECK(input.abs_links[0].range_unbounded);
		EmitAbsConstraints(input, indexer, NativeConstructPolicy {true, false, false});
		// `t = x` restates a column that exists, so each row is the general constraint alone.
		CHECK(input.general_constraints.size() == ABS_ROWS);
		CHECK(input.global_constraints.empty());
		CHECK(BinaryColumns(input) == 0);

		SolverInput lowered_only = AbsInput(false, 1e30);
		EmitAbsEnvelopes(lowered_only);
		auto lowered_indexer = VarIndexer::Build(lowered_only);
		CHECK_THROWS_AS(DeriveAbsAuxiliaryBounds(lowered_only, lowered_indexer, names, /*refuse_when_unbounded=*/true),
		                InvalidInputException);
	}

	SECTION("a forced policy states it natively wherever the backend declares it") {
		SolverInput input = AbsInput(false);
		EmitAbsEnvelopes(input);
		auto indexer = VarIndexer::Build(input);
		DeriveAbsAuxiliaryBounds(input, indexer, names, /*refuse_when_unbounded=*/false);
		EmitAbsConstraints(input, indexer, NativeConstructPolicy {true, true, true});
		CHECK(input.general_constraints.size() == ABS_ROWS);
		CHECK(BinaryColumns(input) == 0);
	}

	SECTION("an ABS its clause pins has the lower envelope only") {
		SolverInput input = AbsInput(false);
		input.abs_links[0].exact = false;
		EmitAbsEnvelopes(input);
		auto indexer = VarIndexer::Build(input);
		EmitAbsConstraints(input, indexer, NativeConstructPolicy {true, true, true});
		CHECK(input.constraints.size() == 2);
		CHECK(input.global_constraints.empty());
		CHECK(input.general_constraints.empty());
	}
}
