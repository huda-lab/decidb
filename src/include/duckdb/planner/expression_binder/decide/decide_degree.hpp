//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/decide/decide_degree.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! Polynomial degree of a DECIDE expression in decision variables, and — at degree 2 —
//! which of the two supported shapes produced it.
//!
//! This is the one definition of degree in DeciDB. Layer 2 owns it because degree is a
//! property of the query, not of what the optimizer later does with it: whether
//! `SUM(POWER(x*y, 2)) <= 10` is a legal DECIDE query must follow from the query and the
//! schema, exactly as the integrality gate does. Layer 5 consumes the same function to
//! assert that its own rewrites preserved what layer 2 admitted — it never decides.
//!
//! Degree is NOT an occurrence count: an additive node contributes the MAX of its terms'
//! degrees, not their sum, because `(x + y) * z` expands to `x*z + y*z` and is degree 2 —
//! three occurrences, but a legal bilinear term.
struct DecideDegree {
	//! Total polynomial degree in decision columns. DECIDE supports 0, 1 and 2; a higher
	//! degree is reported truthfully rather than saturated, so a refusal can name it.
	idx_t degree = 0;
	//! Meaningful only at `degree == 2`. True when the degree came from squaring — one
	//! decision meeting itself (`x*x`, `POWER(x, 2)`, `POWER(x+y, 2)`) — which is the
	//! quadratic form that feeds a Q matrix. False when two *different* decisions were
	//! multiplied (`x*y`), which is the bilinear shape that feeds McCormick envelopes.
	bool is_quadratic_form = false;

	bool IsConstant() const {
		return degree == 0;
	}
	bool IsLinear() const {
		return degree <= 1;
	}
	//! Is this a shape DECIDE can formulate at all?
	bool IsSupported() const {
		return degree <= 2;
	}
};

//! Degree of `expr` in the decision columns bound to `decide_index`.
//!
//! Reads the bound tree, so it sees the casts the binder inserted and the types it
//! resolved; `POWER(x, 2)` binds as `power(CAST(x AS DOUBLE), CAST(2 AS DOUBLE))` and the
//! exponent is read through those casts. An expression whose shape cannot be reasoned
//! about — an unrecognised function wrapping a decision — reports a degree above 2 rather
//! than guessing, because DECIDE cannot formulate what it cannot classify.
DecideDegree DecideExpressionDegree(const Expression &expr, idx_t decide_index);

//! Refuse any constraint whose degree in decisions exceeds what DECIDE can formulate.
//!
//! Runs on the bound tree immediately after `DecideConstraintsBinder`, and is *total*:
//! it descends conjunctions and `WHEN` / `PER` wrappers to every comparison that becomes
//! a model row, and checks both sides. Reducer arguments and bare per-row constraints are
//! therefore judged by one rule — before this existed, `SUM(POWER(x*y,2)) <= 10` was
//! refused at bind time while the identical `POWER(x*y,2) <= 5` reached term extraction
//! and was refused at plan time, in extractor vocabulary and with no source location.
void ValidateDecideConstraintDegree(const Expression &expr, idx_t decide_index);

//! The same refusal for the objective expression, which is an arithmetic tree rather
//! than a comparison.
void ValidateDecideObjectiveDegree(const Expression &expr, idx_t decide_index);

//! Read a constant exponent through the casts the binder inserts: `POWER(x, 2)` binds as
//! `power(CAST(x AS DOUBLE), CAST(2 AS DOUBLE))`, so the literal is never a bare
//! `BoundConstantExpression` by the time a bound-tree pass reaches it. Shared by the
//! degree walk and the integrality gate, which ask the same question of the same node.
bool TryGetDecideConstantExponent(const Expression &expr, double &out_value);

} // namespace duckdb
