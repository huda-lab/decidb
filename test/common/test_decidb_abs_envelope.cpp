#include "catch.hpp"

#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/decidb/formulation/solver_input.hpp"

using namespace duckdb;

// The contract between stage 05 and the ABS lowering of stage 06 (pipeline/06_model_formulation): an envelope row holds
// no aggregate, because stage 05 gives every reducer a function reads a helper decision. The lowering reads each
// envelope row per row, so a row that kept an aggregate is refused where the rows are taken, on SolverInputs built by
// hand: `x` per row (decision 0), the auxiliary `a` (decision 1) and the sign indicator `y` (decision 2).

namespace {

constexpr idx_t ENVELOPE_ROWS = 3;

//! `a - x >= 0` (C1) or `a + x >= 0` (C2), each tagged as the envelope row of auxiliary 1.
EvaluatedConstraint EnvelopeRow(bool positive, bool aggregate) {
	EvaluatedConstraint ec;
	ec.variable_indices = {1, 0};
	ec.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, ENVELOPE_ROWS));
	ec.row_coefficients.push_back(CoefficientColumn::MakeScalar(positive ? -1.0 : 1.0, ENVELOPE_ROWS));
	ec.rhs_values = CoefficientColumn::MakeScalar(0.0, ENVELOPE_ROWS);
	ec.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	ec.abs_aux_idx = 1;
	ec.abs_is_pos_bound = positive;
	ec.lhs_is_aggregate = aggregate;
	return ec;
}

SolverInput EnvelopePairInput(bool aggregate) {
	SolverInput input;
	input.num_rows = ENVELOPE_ROWS;
	input.num_decide_vars = 3;
	input.variable_types.assign(3, LogicalType::DOUBLE);
	input.lower_bounds.assign(3, 0.0);
	input.upper_bounds.assign(3, 1.0);
	input.variable_scopes.assign(3, DecideVarScopeInfo::Row());
	input.constraints.push_back(EnvelopeRow(true, aggregate));
	input.constraints.push_back(EnvelopeRow(false, aggregate));
	input.abs_maximize_links.push_back(AbsMaximizeLinkSpec {1, 2});
	return input;
}

} // namespace

TEST_CASE("DeciDB ABS envelope rows hold no aggregate", "[decidb][formulation][abs]") {
	SECTION("a per-row envelope pair gets its two sign rows") {
		SolverInput input = EnvelopePairInput(false);
		LinearizeAbsMaximize(input);
		CHECK(input.constraints.size() == 4);
	}

	SECTION("an envelope row that holds an aggregate is an internal error") {
		SolverInput input = EnvelopePairInput(true);
		CHECK_THROWS_AS(LinearizeAbsMaximize(input), InternalException);
		CHECK(input.constraints.size() == 2);
	}
}
