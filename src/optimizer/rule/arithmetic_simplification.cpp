#include "duckdb/optimizer/rule/arithmetic_simplification.hpp"

#include <cmath>

#include "duckdb/common/exception.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

ArithmeticSimplificationRule::ArithmeticSimplificationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match on an OperatorExpression that has a ConstantExpression as child
	auto op = make_uniq<FunctionExpressionMatcher>();
	op->matchers.push_back(make_uniq<ConstantExpressionMatcher>());
	op->matchers.push_back(make_uniq<ExpressionMatcher>());
	op->policy = SetMatcher::Policy::SOME;
	// we only match on simple arithmetic expressions (+, -, *, /, //)
	op->function = make_uniq<ManyFunctionMatcher>(unordered_set<string> {"+", "-", "*", "/", "//"});
	// and only with integer or floating results
	op->type = make_uniq<IntegerOrFloatingTypeMatcher>();
	op->matchers[0]->type = make_uniq<IntegerOrFloatingTypeMatcher>();
	op->matchers[1]->type = make_uniq<IntegerOrFloatingTypeMatcher>();
	root = std::move(op);
}

unique_ptr<Expression> ArithmeticSimplificationRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                           bool &changes_made, bool is_root) {
	auto &root = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &constant = bindings[1].get().Cast<BoundConstantExpression>();
	idx_t constant_child = root.children[0].get() == &constant ? 0 : 1;
	D_ASSERT(root.children.size() == 2);
	(void)root;
	// any arithmetic operator involving NULL is always NULL
	if (constant.value.IsNull()) {
		return make_uniq<BoundConstantExpression>(Value(root.return_type));
	}
	auto &func_name = root.function.name;
	if (root.return_type.id() == LogicalTypeId::FLOAT || root.return_type.id() == LogicalTypeId::DOUBLE) {
		return ApplyFloating(root, constant, constant_child);
	}
	if (func_name == "+") {
		if (constant.value == 0) {
			// addition with 0
			// we can remove the entire operator and replace it with the non-constant child
			return std::move(root.children[1 - constant_child]);
		}
	} else if (func_name == "-") {
		if (constant_child == 1 && constant.value == 0) {
			// subtraction by 0
			// we can remove the entire operator and replace it with the non-constant child
			return std::move(root.children[1 - constant_child]);
		}
	} else if (func_name == "*") {
		if (constant.value == 1) {
			// multiply with 1, replace with non-constant child
			return std::move(root.children[1 - constant_child]);
		} else if (constant.value == 0) {
			// multiply by zero: replace with constant or null
			return ExpressionRewriter::ConstantOrNull(std::move(root.children[1 - constant_child]),
			                                          Value::Numeric(root.return_type, 0));
		}
	} else if (func_name == "//") {
		if (constant_child == 1) {
			if (constant.value == 1) {
				// divide by 1, replace with non-constant child
				return std::move(root.children[1 - constant_child]);
			} else if (constant.value == 0) {
				// divide by 0, replace with NULL
				return make_uniq<BoundConstantExpression>(Value(root.return_type));
			}
		}
	} else {
		throw InternalException("Unrecognized function name in ArithmeticSimplificationRule");
	}
	return nullptr;
}

//! The identities that hold bit for bit under IEEE 754, for every value of the operand, NaN, infinities and the sign of
//! zero included: `x * 1`, `x / 1` and `x - 0`. `x + 0` is not one (-0.0 + 0.0 is +0.0), nor is `x * 0` (x may be NaN
//! or infinite), nor `x - -0.0` (it is `x + 0.0`), so a floating type keeps those.
unique_ptr<Expression> ArithmeticSimplificationRule::ApplyFloating(BoundFunctionExpression &root,
                                                                   const BoundConstantExpression &constant,
                                                                   idx_t constant_child) {
	auto &other = root.children[1 - constant_child];
	// the binder casts both operands of a floating operator to its type, so the operand can stand for the expression
	D_ASSERT(other->return_type == root.return_type && constant.return_type == root.return_type);
	auto value = constant.value.GetValue<double>();
	auto &func_name = root.function.name;
	bool identity = false;
	if (func_name == "*") {
		identity = value == 1.0;
	} else if (func_name == "/") {
		identity = constant_child == 1 && value == 1.0;
	} else if (func_name == "-") {
		identity = constant_child == 1 && value == 0.0 && !std::signbit(value);
	}
	return identity ? std::move(other) : nullptr;
}

} // namespace duckdb
