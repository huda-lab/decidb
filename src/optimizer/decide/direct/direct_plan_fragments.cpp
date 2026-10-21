#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
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
	count->children.push_back(
	    make_uniq<BoundCaseExpression>(std::move(predicate), make_uniq<BoundConstantExpression>(Value::INTEGER(1)),
	                                   make_uniq<BoundConstantExpression>(Value(LogicalType::INTEGER))));
	return count;
}

DirectPipelineBreaker DirectValidationBreaker(Binder &binder, unique_ptr<LogicalOperator> input,
                                              unique_ptr<Expression> valid) {
	input->ResolveOperatorTypes();
	auto bindings = input->GetColumnBindings();
	if (bindings.size() != input->types.size() || valid->return_type != LogicalType::BOOLEAN) {
		throw InternalException("Direct solve validation breaker received an invalid input");
	}
	DirectPipelineBreaker result;
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
	window->expressions.push_back(DirectWindowMatchingCount(
	    DirectColumn(LogicalType::BOOLEAN, ColumnBinding(projection_index, bindings.size()))));
	window->children.push_back(std::move(projection));
	result.breaker = ColumnBinding(window_index, 0);
	result.plan = std::move(window);
	return result;
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

unique_ptr<Expression> DirectIsNull(unique_ptr<Expression> value) {
	auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	is_null->children.push_back(std::move(value));
	return std::move(is_null);
}

unique_ptr<Expression> DirectOverProjection(const Expression &value, const vector<ColumnBinding> &source_bindings,
                                            idx_t projection_index) {
	auto copy = value.Copy();
	if (!DirectRemapSourceReferences(*copy, source_bindings, projection_index)) {
		throw InternalException("Direct solve could not re-point a value at the projection that carries the source");
	}
	return copy;
}

unique_ptr<Expression> DirectMergedOverProjection(ClientContext &context, const vector<DirectSignedValue> &values,
                                                  const vector<ColumnBinding> &source_bindings,
                                                  idx_t projection_index) {
	unique_ptr<Expression> merged;
	int first_sign = 1;
	for (auto &value : values) {
		if (!merged) {
			merged = value.expr->Copy();
			first_sign = value.sign;
			continue;
		}
		merged = DecideMergeLikeCoefficients(context, first_sign, std::move(merged), value.sign, value.expr->Copy());
	}
	return merged ? DirectOverProjection(*merged, source_bindings, projection_index) : nullptr;
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
	auto active_predicate = DirectRaiseUnless(
	    std::move(has_active_rows), DirectErrorPredicate(optimizer, DecideEmptyRowSetMessage("aggregate", clause)));
	auto active_guard = make_uniq<LogicalFilter>(std::move(active_predicate));
	active_guard->children.push_back(std::move(active_window));
	return std::move(active_guard);
}

} // namespace duckdb
