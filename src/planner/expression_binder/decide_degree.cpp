#include "duckdb/planner/expression_binder/decide_degree.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <cmath>
#include <unordered_set>

namespace duckdb {

//! Degree DECIDE can formulate. Anything above is refused; the walk reports the true
//! value where it knows it, and this sentinel where the shape is unclassifiable.
static constexpr idx_t DECIDE_MAX_DEGREE = 2;
static constexpr idx_t DECIDE_UNCLASSIFIABLE_DEGREE = DECIDE_MAX_DEGREE + 1;

namespace {

//! Degree plus the decision columns that produced it. The column set is what separates
//! `x*x` from `x*y` at degree 2, and it is why this cannot be a plain integer.
struct DegreeWalk {
	idx_t degree = 0;
	bool is_quadratic_form = false;
	std::unordered_set<idx_t> vars;
};

} // namespace

static bool IsDecisionColumn(const Expression &expr, idx_t decide_index, idx_t &out_column) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &colref = expr.Cast<const BoundColumnRefExpression>();
	if (colref.binding.table_index != decide_index) {
		return false;
	}
	out_column = colref.binding.column_index;
	return true;
}

static bool ContainsDecision(const Expression &expr, idx_t decide_index) {
	idx_t column;
	if (IsDecisionColumn(expr, decide_index, column)) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = ContainsDecision(child, decide_index);
		}
	});
	return found;
}

bool TryGetDecideConstantExponent(const Expression &expr, double &out_value) {
	const Expression *cur = &expr;
	while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		cur = cur->Cast<const BoundCastExpression>().child.get();
	}
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	try {
		out_value =
		    cur->Cast<const BoundConstantExpression>().value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	} catch (...) {
		return false;
	}
	return true;
}

static DegreeWalk WalkDegree(const Expression &expr, idx_t decide_index);

//! Combine children the way an additive node does: degree is the MAX, not the sum.
static void AbsorbAdditive(DegreeWalk &into, const DegreeWalk &child) {
	if (child.degree > into.degree) {
		into.degree = child.degree;
	}
	into.is_quadratic_form = into.is_quadratic_form || child.is_quadratic_form;
	into.vars.insert(child.vars.begin(), child.vars.end());
}

static DegreeWalk WalkMultiplication(const BoundFunctionExpression &func, idx_t decide_index) {
	DegreeWalk result;
	vector<DegreeWalk> children;
	for (auto &child : func.children) {
		children.push_back(WalkDegree(*child, decide_index));
	}
	for (auto &child : children) {
		// A product multiplies degrees together, so here the degrees add.
		result.degree += child.degree;
		result.is_quadratic_form = result.is_quadratic_form || child.is_quadratic_form;
		result.vars.insert(child.vars.begin(), child.vars.end());
	}
	if (result.degree == 2 && !result.is_quadratic_form) {
		// Degree 2 formed from two separate factors. It is the quadratic form when those
		// factors share a decision — `(a + x) * x` expands to `a*x + x*x` — and bilinear
		// when they do not. This is the same test the parsed-tree gate used to make.
		for (idx_t i = 0; i < children.size() && !result.is_quadratic_form; i++) {
			if (children[i].degree == 0) {
				continue;
			}
			for (idx_t j = i + 1; j < children.size() && !result.is_quadratic_form; j++) {
				if (children[j].degree == 0) {
					continue;
				}
				for (auto var : children[i].vars) {
					if (children[j].vars.count(var)) {
						result.is_quadratic_form = true;
						break;
					}
				}
			}
		}
	}
	return result;
}

static DegreeWalk WalkDegree(const Expression &expr, idx_t decide_index) {
	DegreeWalk result;
	idx_t column;
	if (IsDecisionColumn(expr, decide_index, column)) {
		result.degree = 1;
		result.vars.insert(column);
		return result;
	}

	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
	case ExpressionClass::BOUND_CONSTANT:
	case ExpressionClass::BOUND_REF:
		// Data, not a decision: degree 0 whatever its type.
		return result;
	case ExpressionClass::BOUND_CAST:
		return WalkDegree(*expr.Cast<const BoundCastExpression>().child, decide_index);
	case ExpressionClass::BOUND_AGGREGATE: {
		// SUM / AVG / MIN / MAX and the norm marker combine values across rows without
		// multiplying them, so a reducer has the degree of what it reduces. Its `filter`
		// (an aggregate-local WHEN) may not reference a decision and is not walked.
		auto &agg = expr.Cast<const BoundAggregateExpression>();
		for (auto &child : agg.children) {
			AbsorbAdditive(result, WalkDegree(*child, decide_index));
		}
		return result;
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		// A tagged conjunction is a DECIDE wrapper, not a boolean AND: `WHEN` and `PER`
		// carry the term in children[0] and the condition / grouping columns after it,
		// and those may not reference a decision at all. An untagged conjunction is a
		// boolean, which is not an arithmetic term and carries no degree.
		auto &conj = expr.Cast<const BoundConjunctionExpression>();
		if ((HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG) || IsPerConstraintTag(conj.alias)) &&
		    !conj.children.empty()) {
			return WalkDegree(*conj.children[0], decide_index);
		}
		return result;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<const BoundFunctionExpression>();
		auto name = StringUtil::Lower(func.function.name);

		if (name == "+" || name == "-") {
			for (auto &child : func.children) {
				AbsorbAdditive(result, WalkDegree(*child, decide_index));
			}
			return result;
		}
		if (name == "*") {
			return WalkMultiplication(func, decide_index);
		}
		if (name == "/" && func.children.size() == 2) {
			// Dividing by a decision is not polynomial at all. The binder refuses it
			// separately with a better sentence; reporting it unclassifiable here keeps
			// this function total without letting the shape through.
			if (ContainsDecision(*func.children[1], decide_index)) {
				result.degree = DECIDE_UNCLASSIFIABLE_DEGREE;
				return result;
			}
			return WalkDegree(*func.children[0], decide_index);
		}
		if (name == "pow" || name == "power" || name == "**" || name == "^") {
			double exponent;
			if (func.children.size() == 2 && TryGetDecideConstantExponent(*func.children[1], exponent) &&
			    exponent >= 0.0 && exponent == std::floor(exponent)) {
				auto base = WalkDegree(*func.children[0], decide_index);
				result.vars = base.vars;
				result.degree = base.degree * static_cast<idx_t>(exponent);
				// Squaring a decision-bearing base is the quadratic form, whatever the
				// base contains: `POWER(x + y, 2)` expands to x² + 2xy + y², which a Q
				// matrix represents and McCormick does not.
				result.is_quadratic_form =
				    base.degree >= 1 && (exponent == 2.0 || base.is_quadratic_form);
				return result;
			}
			// A fractional, negative or non-constant exponent is not a polynomial degree.
			result.degree = ContainsDecision(expr, decide_index) ? DECIDE_UNCLASSIFIABLE_DEGREE : 0;
			return result;
		}
		if (name == "abs") {
			// ABS is linearized by layer 5 into rows that are as linear as its argument,
			// so it changes magnitude, not degree.
			for (auto &child : func.children) {
				AbsorbAdditive(result, WalkDegree(*child, decide_index));
			}
			return result;
		}
		// Any other function over a decision is not a polynomial shape DECIDE can
		// formulate. `ValidateDecideNoNonLinearScalar` refuses these on the parsed tree
		// with a sentence naming the function, so this is a backstop, not the diagnosis.
		result.degree = ContainsDecision(expr, decide_index) ? DECIDE_UNCLASSIFIABLE_DEGREE : 0;
		return result;
	}
	default:
		break;
	}

	// An unrecognised node is degree 0 when it holds no decision, and unclassifiable when
	// it does — DECIDE cannot formulate a shape it cannot name.
	result.degree = ContainsDecision(expr, decide_index) ? DECIDE_UNCLASSIFIABLE_DEGREE : 0;
	return result;
}

DecideDegree DecideExpressionDegree(const Expression &expr, idx_t decide_index) {
	auto walk = WalkDegree(expr, decide_index);
	DecideDegree result;
	result.degree = walk.degree;
	result.is_quadratic_form = walk.is_quadratic_form;
	return result;
}

//! Refuse one side of a comparison, or a whole objective, whose degree is too high.
static void RefuseUnsupportedDegree(const Expression &expr, const DecideDegree &degree) {
	// The expression is deliberately not echoed. `ToString()` on a bound tree prints the
	// casts the binder inserted (`CAST(x AS DECIMAL(18,0))`), which is not what the user
	// wrote; the source location carries a caret to the term instead.
	throw BinderException(
	    expr,
	    "Triple or higher-order products of DECIDE variables are not supported (total degree > 2). "
	    "This term reaches degree %llu. A term may use one decision, one decision squared "
	    "(POWER(x, 2)), or two different decisions multiplied (x * y).",
	    static_cast<uint64_t>(degree.degree));
}

static void ValidateSideDegree(const Expression &expr, idx_t decide_index) {
	auto degree = DecideExpressionDegree(expr, decide_index);
	if (!degree.IsSupported()) {
		RefuseUnsupportedDegree(expr, degree);
	}
}

void ValidateDecideConstraintDegree(const Expression &expr, idx_t decide_index) {
	// Constraint position only, and by the same descent the integrality gate uses: a
	// comparison nested inside an operand is a boolean value, not a model row.
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<const BoundConjunctionExpression>();
		if (HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG) || IsPerConstraintTag(conj.alias)) {
			// children[0] is the constraint; the rest are the condition / PER columns,
			// which may not reference a decision at all.
			if (!conj.children.empty()) {
				ValidateDecideConstraintDegree(*conj.children[0], decide_index);
			}
			return;
		}
		for (const auto &child : conj.children) {
			ValidateDecideConstraintDegree(*child, decide_index);
		}
		return;
	}
	case ExpressionClass::BOUND_COMPARISON: {
		auto &comp = expr.Cast<const BoundComparisonExpression>();
		// Both sides: canonicalization has not run, so `5 > x*y*z` is as likely a
		// spelling as `x*y*z < 5`.
		ValidateSideDegree(*comp.left, decide_index);
		ValidateSideDegree(*comp.right, decide_index);
		return;
	}
	default:
		// A per-row constraint that is not a comparison (a bound `IN` operator, a bare
		// boolean decision) is degree 1 by construction and has nothing to check.
		return;
	}
}

void ValidateDecideObjectiveDegree(const Expression &expr, idx_t decide_index) {
	// An objective is one arithmetic tree, so there is no structure to descend here: the
	// `WHEN` / `PER` wrappers it may carry are peeled by the walk itself.
	ValidateSideDegree(expr, decide_index);
}

} // namespace duckdb
