#include "duckdb/optimizer/decide/direct/direct_builder.hpp"

#include <algorithm>

#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

unique_ptr<Expression> DirectColumn(const LogicalType &type, ColumnBinding binding) {
	return make_uniq<BoundColumnRefExpression>(type, binding);
}

unique_ptr<Expression> DirectConstantBool(bool value) {
	return make_uniq<BoundConstantExpression>(Value::BOOLEAN(value));
}

unique_ptr<Expression> DirectAnyCondition(const vector<unique_ptr<Expression>> &conditions) {
	unique_ptr<Expression> result = DirectConstantBool(false);
	for (auto &condition : conditions) {
		result = make_uniq<BoundCaseExpression>(condition->Copy(), DirectConstantBool(true), std::move(result));
	}
	return result;
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

unique_ptr<BoundWindowExpression> DirectWindowExtremum(Optimizer &optimizer, const char *name,
                                                unique_ptr<Expression> value) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(value));
	auto bound = optimizer.BindAggregateFunction(name, std::move(children));
	auto &aggregate = bound->Cast<BoundAggregateExpression>();
	auto result = make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_AGGREGATE, aggregate.return_type,
	                                                make_uniq<AggregateFunction>(aggregate.function),
	                                                std::move(aggregate.bind_info));
	result->children = std::move(aggregate.children);
	result->start = WindowBoundary::UNBOUNDED_PRECEDING;
	result->end = WindowBoundary::UNBOUNDED_FOLLOWING;
	return result;
}

unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, const string &message) {
	auto error = optimizer.BindScalarFunction("error", make_uniq<BoundConstantExpression>(Value(message)));
	return BoundCastExpression::AddCastToType(optimizer.context, std::move(error), LogicalType::BOOLEAN);
}

unique_ptr<Expression> DirectValidScorePredicate(Optimizer &optimizer, const LogicalType &score_type,
                                                 ColumnBinding score_binding) {
	auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	is_null->children.push_back(DirectColumn(score_type, score_binding));
	auto is_finite = optimizer.BindScalarFunction("isfinite", DirectColumn(score_type, score_binding));
	auto finite_or_error = make_uniq<BoundCaseExpression>(
	    std::move(is_finite), DirectConstantBool(true),
	    DirectErrorPredicate(optimizer, "Direct solve coefficient is non-finite"));
	return make_uniq<BoundCaseExpression>(std::move(is_null),
	                                      DirectErrorPredicate(optimizer, "Direct solve coefficient is NULL"),
	                                      std::move(finite_or_error));
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

} // namespace duckdb
