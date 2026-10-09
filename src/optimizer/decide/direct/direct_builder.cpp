#include "duckdb/optimizer/decide/direct/direct_builder.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"

#include <algorithm>

#include "duckdb/common/string_util.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

unique_ptr<Expression> DirectColumn(const LogicalType &type, ColumnBinding binding) {
	return make_uniq<BoundColumnRefExpression>(type, binding);
}

unique_ptr<Expression> DirectConstantBool(bool value) {
	return make_uniq<BoundConstantExpression>(Value::BOOLEAN(value));
}

unique_ptr<BoundWindowExpression> DirectWindowMatchingCount(unique_ptr<Expression> predicate) {
	auto count = make_uniq<BoundWindowExpression>(
	    ExpressionType::WINDOW_AGGREGATE, LogicalType::BIGINT,
	    make_uniq<AggregateFunction>(CountFun::GetFunctions().GetFunctionByOffset(0)), nullptr);
	count->start = WindowBoundary::UNBOUNDED_PRECEDING;
	count->end = WindowBoundary::UNBOUNDED_FOLLOWING;
	count->children.push_back(make_uniq<BoundCaseExpression>(
	    std::move(predicate), make_uniq<BoundConstantExpression>(Value::INTEGER(1)),
	    make_uniq<BoundConstantExpression>(Value(LogicalType::INTEGER))));
	return count;
}

DirectBarrier DirectValidationBarrier(Binder &binder, unique_ptr<LogicalOperator> input, unique_ptr<Expression> valid) {
	input->ResolveOperatorTypes();
	auto bindings = input->GetColumnBindings();
	if (bindings.size() != input->types.size() || valid->return_type != LogicalType::BOOLEAN) {
		throw InternalException("Direct solve validation barrier received an invalid input");
	}
	DirectBarrier result;
	auto projection_index = binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> expressions;
	for (idx_t i = 0; i < bindings.size(); i++) {
		expressions.push_back(DirectColumn(input->types[i], bindings[i]));
		result.input_bindings.emplace_back(projection_index, i);
	}
	expressions.push_back(std::move(valid));
	auto projection = make_uniq<LogicalProjection>(projection_index, std::move(expressions));
	projection->children.push_back(std::move(input));
	auto window_index = binder.GenerateTableIndex();
	auto window = make_uniq<LogicalWindow>(window_index);
	window->expressions.push_back(
	    DirectWindowMatchingCount(DirectColumn(LogicalType::BOOLEAN, ColumnBinding(projection_index, bindings.size()))));
	window->children.push_back(std::move(projection));
	result.barrier = ColumnBinding(window_index, 0);
	result.plan = std::move(window);
	return result;
}

unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, unique_ptr<Expression> message) {
	auto error = optimizer.BindScalarFunction("error", std::move(message));
	return BoundCastExpression::AddCastToType(optimizer.context, std::move(error), LogicalType::BOOLEAN);
}

unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, const string &message) {
	return DirectErrorPredicate(optimizer, make_uniq<BoundConstantExpression>(Value(message)));
}

unique_ptr<Expression> DirectScaledTerm(Optimizer &optimizer, unique_ptr<Expression> value, int sign, bool scaled,
                                        double scale, bool divides) {
	auto term = BoundCastExpression::AddCastToType(optimizer.context, std::move(value), LogicalType::DOUBLE);
	if (sign < 0) {
		term =
		    optimizer.BindScalarFunction("*", std::move(term), make_uniq<BoundConstantExpression>(Value::DOUBLE(-1.0)));
	}
	if (scaled) {
		term = optimizer.BindScalarFunction(divides ? "/" : "*", std::move(term),
		                                    make_uniq<BoundConstantExpression>(Value::DOUBLE(scale)));
	}
	return term;
}

unique_ptr<Expression> DirectInfeasibleErrorPredicate(Optimizer &optimizer) {
	return DirectErrorPredicate(optimizer, "DECIDE optimization is infeasible. Prefix the query with DIAGNOSE to see "
	                                       "which clause to change.");
}

unique_ptr<Expression> DirectUnboundedErrorPredicate(Optimizer &optimizer) {
	return DirectErrorPredicate(optimizer, "DECIDE optimization is unbounded. Prefix the query with DIAGNOSE to see "
	                                       "which decision needs a bound.");
}

static unique_ptr<Expression> DirectVarchar(const string &text) {
	return make_uniq<BoundConstantExpression>(Value(text));
}

static unique_ptr<Expression> DirectBigint(int64_t value) {
	return make_uniq<BoundConstantExpression>(Value::BIGINT(value));
}

unique_ptr<Expression> DirectIsNull(unique_ptr<Expression> value) {
	auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	is_null->children.push_back(std::move(value));
	return std::move(is_null);
}

//! The error for a NaN or infinite number, naming the kind of coefficient the user wrote it in.
static unique_ptr<Expression> DirectInvalidNumberError(Optimizer &optimizer, DirectNumberKind kind) {
	return DirectErrorPredicate(optimizer,
	                            StringUtil::Format("DECIDE %s coefficient contains invalid value (NaN or "
	                                               "Infinity). Common causes:\n"
	                                               "  • Division by zero in the expression\n"
	                                               "  • Arithmetic overflow in calculations\n"
	                                               "  • NULL values that propagated through math "
	                                               "operations\n"
	                                               "Check your expressions and input data.",
	                                               kind == DirectNumberKind::OBJECTIVE ? "objective" : "constraint"));
}

//! The NULL message for a number, as an expression over the row it is evaluated on. It spells out what the solver
//! prints for the same row: the NULL column, or the NULL columns, or the number's own text when no column is NULL.
unique_ptr<Expression> DirectValidNumberPredicate(Optimizer &optimizer, const LogicalType &number_type,
                                                  ColumnBinding number_binding, const string &number_text,
                                                  DirectNumberKind kind) {
	const string advice = "Impute it with COALESCE(), or filter those rows out with a WHERE clause.";
	auto null_message = number_text.empty() ? "DECIDE: a value used in the optimization is NULL. " + advice
	                                        : StringUtil::Format("DECIDE: %s is NULL. %s", number_text, advice);
	auto is_null = DirectIsNull(DirectColumn(number_type, number_binding));
	auto is_finite = optimizer.BindScalarFunction("isfinite", DirectColumn(number_type, number_binding));
	auto finite_or_error = make_uniq<BoundCaseExpression>(std::move(is_finite), DirectConstantBool(true),
	                                                      DirectInvalidNumberError(optimizer, kind));
	return make_uniq<BoundCaseExpression>(std::move(is_null), DirectErrorPredicate(optimizer, null_message),
	                                      std::move(finite_or_error));
}

unique_ptr<Expression> DirectValidBoundPredicate(Optimizer &optimizer, ColumnBinding bound_binding,
                                                 const string &column_name) {
	auto message = [&]() {
		return DirectErrorPredicate(optimizer, DirectInvalidBoundMessage(column_name));
	};
	auto is_nan = optimizer.BindScalarFunction("isnan", DirectColumn(LogicalType::DOUBLE, bound_binding));
	auto not_nan = make_uniq<BoundCaseExpression>(std::move(is_nan), message(), DirectConstantBool(true));
	return make_uniq<BoundCaseExpression>(DirectIsNull(DirectColumn(LogicalType::DOUBLE, bound_binding)), message(),
	                                      std::move(not_nan));
}

bool DirectCanSkipSourceOutput(LogicalOperator &source, ColumnBinding binding) {
	switch (source.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = source.Cast<LogicalGet>();
		if (get.function.function != TableScanFunction::GetFunction().function || !get.GetTable()) {
			return false;
		}
		auto bindings = get.GetColumnBindings();
		return std::find(bindings.begin(), bindings.end(), binding) != bindings.end();
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &projection = source.Cast<LogicalProjection>();
		if (binding.table_index != projection.table_index || binding.column_index >= projection.expressions.size()) {
			return false;
		}
		auto &expression = *projection.expressions[binding.column_index];
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			return true;
		}
		if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF || source.children.size() != 1) {
			return false;
		}
		auto &child_ref = expression.Cast<BoundColumnRefExpression>();
		return child_ref.depth == 0 && DirectCanSkipSourceOutput(*source.children[0], child_ref.binding);
	}
	case LogicalOperatorType::LOGICAL_FILTER:
		return source.children.size() == 1 && DirectCanSkipSourceOutput(*source.children[0], binding);
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = source.Cast<LogicalComparisonJoin>();
		if (join.join_type != JoinType::INNER || source.children.size() != 2) {
			return false;
		}
		auto output_bindings = source.GetColumnBindings();
		if (std::find(output_bindings.begin(), output_bindings.end(), binding) == output_bindings.end()) {
			return false;
		}
		auto left_bindings = source.children[0]->GetColumnBindings();
		auto right_bindings = source.children[1]->GetColumnBindings();
		bool on_left = std::find(left_bindings.begin(), left_bindings.end(), binding) != left_bindings.end();
		bool on_right = std::find(right_bindings.begin(), right_bindings.end(), binding) != right_bindings.end();
		if (on_left == on_right) {
			return false;
		}
		return DirectCanSkipSourceOutput(*source.children[on_right ? 1 : 0], binding);
	}
	default:
		return false;
	}
}

//===--------------------------------------------------------------------===//
// DECIDE semantics every aggregate rule shares
//===--------------------------------------------------------------------===//

unique_ptr<Expression> DirectActiveWhen(const Expression *when) {
	return when ? make_uniq<BoundCaseExpression>(when->Copy(), DirectConstantBool(true), DirectConstantBool(false))
	            : DirectConstantBool(true);
}

unique_ptr<LogicalOperator> DirectGuardEmptyAggregate(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                                      ColumnBinding eligible, const string &clause) {
	auto active_window_index = optimizer.binder.GenerateTableIndex();
	auto active_count = DirectWindowMatchingCount(DirectColumn(LogicalType::BOOLEAN, eligible));
	auto active_window = make_uniq<LogicalWindow>(active_window_index);
	active_window->expressions.push_back(std::move(active_count));
	active_window->children.push_back(std::move(input));
	auto has_active_rows = make_uniq<BoundComparisonExpression>(
	    ExpressionType::COMPARE_GREATERTHAN, DirectColumn(LogicalType::BIGINT, ColumnBinding(active_window_index, 0)),
	    make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
	auto active_predicate = make_uniq<BoundCaseExpression>(
	    std::move(has_active_rows), DirectConstantBool(true),
	    DirectErrorPredicate(optimizer, "DECIDE empty row set for aggregate in " + clause +
	                                        ". An empty aggregate has no well-defined value; check your WHEN clause."));
	auto active_guard = make_uniq<LogicalFilter>(std::move(active_predicate));
	active_guard->children.push_back(std::move(active_window));
	return std::move(active_guard);
}

string DirectInvalidBoundMessage(const string &column_name) {
	const string advice = "or filter those rows out with a WHERE clause.";
	if (column_name.empty()) {
		return "DECIDE: the bound expression has an invalid value (NULL or NaN). Impute it with COALESCE(), " + advice;
	}
	return StringUtil::Format("DECIDE: column \"%s\" has an invalid value (NULL or NaN). Impute it with "
	                          "COALESCE(%s, 0), %s",
	                          column_name, column_name, advice);
}

} // namespace duckdb
