//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/planner/decide/decide_product_form.hpp
//
// The one spelling of a product of decisions, and the readers of it.
//===----------------------------------------------------------------------===//
//
// THE SPELLING. Every product of decisions is written as a coefficient on the left
// (a constant, data, or a product of data factors) times a product of factors, each
// a linear form or an opaque term raised to a whole number:
//
//   2*x*x          ->  2 * POWER(x, 2)
//   x*w*x          ->  w * POWER(x, 2)
//   (x-t)*(x-t)    ->  POWER(x - t, 2)
//   (x+y)*(y+x)    ->  POWER(x + y, 2)
//   (b*x)*(b*x)    ->  (b * b) * POWER(x, 2)
//   w*POWER(x, 0)  ->  w + 0 * x
//
// NormalizeProducts is a bottom-up rewrite rule over every decision-bearing `*` and
// whole-exponent POWER node, as DuckDB's ArithmeticSimplificationRule is over constant
// arithmetic. It flattens `*`, the binder's casts over decisions, unary minus and
// POWER(b, k) for a whole constant k; merges equal decision-bearing factors into powers;
// composes data factors into the coefficient with `*`, never merging or folding them, so
// SQL's arithmetic over the data is the one written; and never distributes over a sum.
// A POWER whose base has degree 1 keeps its base whole: POWER(b*x, 2) is one linear
// form squared. Every other node (EXP(x), POWER(x, 0.5), x / y, CASE, ABS, a reducer, a
// subquery) is a factor that is never opened, so the rule is total.
//
// IDENTITY. Each rebuilt node is equal to the node it replaces for every input: it is
// cast back to that node's type and keeps its alias and source location, so a message or a diagnosis quotes what the
// user wrote (the location rule, language/errors.md) while a plan's canonical layer shows the spelling. The tree is
// rebuilt from the factor list in first-seen order, as a left-deep chain, so a second
// pass is a fixed point.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class ClientContext;

//! One factor of a product: `base` raised to `power` (at least 1). `base` is borrowed and has no binder cast on top.
struct DecideProductFactor {
	const Expression *base = nullptr;
	idx_t power = 1;
};

//! A product read as the spelling above: `sign * coefficient * factors`.
struct DecideProductForm {
	int sign = 1;
	//! Decision-free factors, in first-seen order. Empty means 1.
	vector<DecideProductFactor> coefficient;
	//! Decision-bearing factors, in first-seen order, equal ones merged.
	vector<DecideProductFactor> factors;
	//! The decisions of factors raised to 0 (a column, or a reducer kept whole): the product adds each at coefficient
	//! 0, so a clause keeps the decisions it was written over, as the merge keeps `x - x` as `0 * x`.
	vector<const Expression *> zero_terms;
};

//! The degree of an expression in the decisions bound to `decide_index`. `polynomial` is false when a decision stands
//! under a node outside the vocabulary of the model (anything but +, -, *, a division by data, a whole-exponent POWER,
//! ABS and a reducer); `degree` then counts only the parts that are in it.
struct DecideTermDegree {
	idx_t degree = 0;
	bool polynomial = true;

	bool IsLinear() const {
		return polynomial && degree <= 1;
	}
};

DecideTermDegree DecideDegreeOf(ClientContext &context, const Expression &expr, idx_t decide_index);

//! True when `exponent` is known before the query runs and is a whole number (0 included).
bool TryGetDecideWholeExponent(ClientContext &context, const Expression &exponent, idx_t &out);

//! True for a `pow`, `power`, `**` or `^` call with two arguments.
bool IsDecidePowerCall(const Expression &expr);

//! A decision-bearing `*`, or a POWER with a whole exponent: the nodes the spelling rewrites and its readers read.
bool IsDecideProductNode(ClientContext &context, const Expression &expr, idx_t decide_index);

//! `expr` read as a product: flattened through `*`, the binder's casts over decisions, unary minus and whole-exponent
//! POWERs, with equal decision-bearing factors merged. Any decision-bearing expression reads as a product, of one
//! factor when it is nothing else.
DecideProductForm ReadDecideProduct(ClientContext &context, const Expression &expr, idx_t decide_index);

//! Whether two terms are the same: equal trees whose decision-bearing nodes carry the same tags (a reducer's `by` and
//! `per` live in its alias). Tags on data only say where it was written, so they may differ.
bool DecideTermsEqual(const Expression &left, const Expression &right, idx_t decide_index);

//! Whether two decision-bearing factors are the same term: equal trees, or additive forms with the same signed terms
//! (`x + y` and `y + x`).
bool DecideFactorsEqual(const Expression &left, const Expression &right, idx_t decide_index);

//! Whether two products have the same decision-bearing factors with the same powers, whatever their order.
bool DecideFactorListsEqual(const vector<DecideProductFactor> &left, const vector<DecideProductFactor> &right,
                            idx_t decide_index);

//! The coefficient of `form` as a left-deep chain of its factors; null when it has none (the coefficient is 1).
unique_ptr<Expression> BuildDecideProductCoefficient(ClientContext &context, const DecideProductForm &form);

//! `form` written as the spelling: a left-deep chain of the coefficient's factors, then the decision-bearing ones,
//! under a unary minus for a negative sign, plus `0 * t` for each of its zero terms.
unique_ptr<Expression> BuildDecideProductForm(ClientContext &context, const DecideProductForm &form);

//! A copy of `expr` with every decision-bearing product written in the one spelling.
unique_ptr<Expression> NormalizeProducts(ClientContext &context, idx_t decide_index, const Expression &expr);
void NormalizeProductsInPlace(ClientContext &context, idx_t decide_index, unique_ptr<Expression> &expr);

//! The product `left * right` in the one spelling: the builder every pass that multiplies a decision-bearing term
//! uses, so what it builds is already canonical.
unique_ptr<Expression> BuildDecideProduct(ClientContext &context, idx_t decide_index, unique_ptr<Expression> left,
                                          unique_ptr<Expression> right);

} // namespace duckdb
