#include "duckdb/planner/decide/decide_term_split.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/decide/decide_product_form.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Rebuilding a coefficient
//===--------------------------------------------------------------------===//

//! Rebuilding a `BoundFunctionExpression` by hand — reusing another node's
//! `function` / `return_type` / `bind_info` over different children — does not
//! fail when the types disagree. It reinterprets the children's *physical*
//! representation, which silently yields a wrong number and can read past the
//! end of a narrower vector. DECIDE has hit that failure mode three times, always
//! where a subtree was rebuilt after terms were dropped or distributed. Binding
//! through `FunctionBinder` picks the implementation for these argument types,
//! computes the matching return type and bind data, and inserts whatever casts the
//! signature needs.
unique_ptr<Expression> DecideRebindOperator(ClientContext &context, const string &name,
                                            vector<unique_ptr<Expression>> children) {
	FunctionBinder function_binder(context);
	ErrorData error;
	auto result = function_binder.BindScalarFunction(DEFAULT_SCHEMA, name, std::move(children), error);
	if (error.HasError()) {
		throw InternalException("DECIDE failed to rebind '%s' while rebuilding a coefficient: %s", name,
		                        error.Message());
	}
	return result;
}

unique_ptr<Expression> DecideMergeLikeCoefficients(ClientContext &context, int first_sign, unique_ptr<Expression> first,
                                                   int next_sign, unique_ptr<Expression> next) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(first));
	children.push_back(std::move(next));
	return DecideRebindOperator(context, first_sign == next_sign ? "+" : "-", std::move(children));
}

static unique_ptr<Expression> RebindMultiply(ClientContext &context, unique_ptr<Expression> lhs,
                                             unique_ptr<Expression> rhs) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(lhs));
	children.push_back(std::move(rhs));
	return DecideRebindOperator(context, "*", std::move(children));
}

//===--------------------------------------------------------------------===//
// Structural helpers
//===--------------------------------------------------------------------===//

static bool IsAbsFunction(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	       StringUtil::Lower(expr.Cast<BoundFunctionExpression>().function.name) == "abs" &&
	       expr.Cast<BoundFunctionExpression>().children.size() == 1;
}

bool DecideSplitTerm::IsAbs() const {
	return opaque && IsAbsFunction(*opaque);
}

//! `left * right` for two coefficients, either of which may be null (1). A bare decision's literal 1 stays out of the
//! product, so `w * x` reads `w`: a product of a coefficient and 1 could overflow where the coefficient cannot.
static unique_ptr<Expression> MultiplyCoefficients(ClientContext &context, const Expression *left,
                                                   unique_ptr<Expression> right) {
	if (!left) {
		return right;
	}
	if (!right || (right->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
	               right->Cast<BoundConstantExpression>().value == Value::INTEGER(1))) {
		return left->Copy();
	}
	return RebindMultiply(context, left->Copy(), std::move(right));
}

//===--------------------------------------------------------------------===//
// DecideTermSplitter
//===--------------------------------------------------------------------===//

DecideTermSplitter::DecideTermSplitter(ClientContext &context, idx_t decide_index,
                                       const vector<unique_ptr<Expression>> &decide_variables)
    : context(context), decide_index(decide_index), decide_variables(decide_variables) {
	for (idx_t i = 0; i < decide_variables.size(); i++) {
		variable_indexes[decide_variables[i]->Cast<BoundColumnRefExpression>().binding] = i;
	}
}

idx_t DecideTermSplitter::FindVariable(const Expression &expr) const {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto entry = variable_indexes.find(expr.Cast<BoundColumnRefExpression>().binding);
		if (entry != variable_indexes.end()) {
			return entry->second;
		}
	}
	idx_t result = DConstants::INVALID_INDEX;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (result == DConstants::INVALID_INDEX) {
			result = FindVariable(child);
		}
	});
	return result;
}

void DecideTermSplitter::AddOpaque(const Expression &expr, int sign, vector<DecideSplitTerm> &out) const {
	DecideSplitTerm term;
	term.sign = sign;
	term.degree = DecideDegreeOf(context, expr, decide_index).degree;
	term.opaque = expr.Copy();
	if (IsAbsFunction(expr)) {
		Split(*expr.Cast<BoundFunctionExpression>().children[0], 1, term.inner);
	}
	out.push_back(std::move(term));
}

//! A product in the one spelling: its coefficient, then its decision factors. A coefficient on one factor is
//! distributed over that factor's terms; two factors, or one squared, are a term of degree 2.
void DecideTermSplitter::ReadProduct(const Expression &expr, int sign, vector<DecideSplitTerm> &out) const {
	auto form = ReadDecideProduct(context, expr, decide_index);
	auto coefficient = BuildDecideProductCoefficient(context, form);
	sign *= form.sign;
	// A factor raised to 0 keeps its decisions at coefficient 0 (ReadDecideProduct's zero terms).
	for (auto *zero : form.zero_terms) {
		auto begin = out.size();
		Split(*zero, sign, out);
		for (idx_t i = begin; i < out.size(); i++) {
			out[i].coefficient = make_uniq<BoundConstantExpression>(Value::INTEGER(0));
		}
	}
	idx_t degree = 0;
	for (auto &factor : form.factors) {
		degree += factor.power * DecideDegreeOf(context, *factor.base, decide_index).degree;
	}
	if (form.factors.empty()) {
		DecideSplitTerm term;
		term.sign = sign;
		term.coefficient = coefficient ? std::move(coefficient) : make_uniq<BoundConstantExpression>(Value::INTEGER(1));
		out.push_back(std::move(term));
		return;
	}
	if (form.factors.size() == 1 && form.factors[0].power == 1) {
		auto begin = out.size();
		Split(*form.factors[0].base, sign, out);
		for (idx_t i = begin; i < out.size(); i++) {
			out[i].coefficient = MultiplyCoefficients(context, coefficient.get(), std::move(out[i].coefficient));
		}
		return;
	}
	if (degree != 2) {
		throw InternalException("DECIDE: a term of degree %llu reached the term reader; the shape check refuses a "
		                        "degree above 2, so a rewrite built it",
		                        static_cast<uint64_t>(degree));
	}
	DecideSplitTerm term;
	term.sign = sign;
	term.coefficient = std::move(coefficient);
	term.degree = 2;
	for (auto &factor : form.factors) {
		DecideSplitFactor read;
		read.power = factor.power;
		Split(*factor.base, 1, read.form);
		term.factors.push_back(std::move(read));
	}
	out.push_back(std::move(term));
}

//! Division by a decision-free expression: read the numerator and divide every coefficient. Each side is cast to the
//! `/` function's argument types so an integer coefficient does not turn into integer division (`x/2` gave 0 when the
//! coefficient was INT 1), then bound for the operands it has.
void DecideTermSplitter::ReadQuotient(const BoundFunctionExpression &quotient, int sign,
                                      vector<DecideSplitTerm> &out) const {
	auto begin = out.size();
	Split(*quotient.children[0], sign, out);
	D_ASSERT(quotient.function.arguments.size() == 2);
	const auto &num_type = quotient.function.arguments[0];
	const auto &denom_type = quotient.function.arguments[1];
	for (idx_t i = begin; i < out.size(); i++) {
		auto numerator = out[i].coefficient ? std::move(out[i].coefficient)
		                                    : make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
		vector<unique_ptr<Expression>> children;
		children.push_back(BoundCastExpression::AddDefaultCastToType(std::move(numerator), num_type));
		children.push_back(BoundCastExpression::AddDefaultCastToType(quotient.children[1]->Copy(), denom_type));
		out[i].coefficient = DecideRebindOperator(context, "/", std::move(children));
	}
}

void DecideTermSplitter::Split(const Expression &expr, int sign, vector<DecideSplitTerm> &out) const {
	if (FindVariable(expr) == DConstants::INVALID_INDEX) {
		// A decision-free term, a decision-free cast included: a real DuckDB value operation, kept as written.
		DecideSplitTerm term;
		term.sign = sign;
		term.coefficient = expr.Copy();
		out.push_back(std::move(term));
		return;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF: {
		DecideSplitTerm term;
		term.sign = sign;
		term.degree = 1;
		term.var_a = FindVariable(expr);
		term.coefficient = make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
		out.push_back(std::move(term));
		return;
	}
	case ExpressionClass::BOUND_CAST:
		// A decision-bearing cast is DuckDB's type reconciliation; a cast the user wrote was refused at binding.
		Split(*expr.Cast<BoundCastExpression>().child, sign, out);
		return;
	case ExpressionClass::BOUND_FUNCTION:
		break;
	default:
		AddOpaque(expr, sign, out);
		return;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	auto &name = func.function.name;
	if (name == "+") {
		for (auto &child : func.children) {
			Split(*child, sign, out);
		}
	} else if (name == "-" && func.children.size() == 2) {
		Split(*func.children[0], sign, out);
		Split(*func.children[1], -sign, out);
	} else if (name == "-" && func.children.size() == 1) {
		Split(*func.children[0], -sign, out);
	} else if (IsDecideProductNode(context, func, decide_index)) {
		ReadProduct(func, sign, out);
	} else if (name == "/" && func.children.size() == 2 &&
	           FindVariable(*func.children[1]) == DConstants::INVALID_INDEX) {
		ReadQuotient(func, sign, out);
	} else {
		AddOpaque(func, sign, out);
	}
}

} // namespace duckdb
