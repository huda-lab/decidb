#include "s1_support.hpp"

#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/optimizer/decide/direct/direct_builder.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {
namespace direct_s1 {

unique_ptr<Expression> DirectAnyCondition(const vector<unique_ptr<Expression>> &conditions) {
	unique_ptr<Expression> result = DirectConstantBool(false);
	for (auto &condition : conditions) {
		result = make_uniq<BoundCaseExpression>(condition->Copy(), DirectConstantBool(true), std::move(result));
	}
	return result;
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

DirectScopeState DirectProjectScope(Binder &binder, unique_ptr<LogicalOperator> input, bool scoped,
                                    const Expression *when, const vector<idx_t> &key_slots,
                                    vector<unique_ptr<Expression>> extra) {
	input->ResolveOperatorTypes();
	auto bindings = input->GetColumnBindings();
	auto &types = input->types;
	DirectScopeState state;
	state.input_bindings = bindings;
	if (!scoped && extra.empty()) {
		state.plan = std::move(input);
		return state;
	}
	state.state_index = binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> expressions;
	for (idx_t i = 0; i < bindings.size(); i++) {
		expressions.push_back(DirectColumn(types[i], bindings[i]));
		state.input_bindings[i] = ColumnBinding(state.state_index, i);
	}
	if (scoped) {
		unique_ptr<Expression> eligible = DirectActiveWhen(when);
		for (auto slot : key_slots) {
			if (slot >= bindings.size()) {
				throw InternalException("Direct solve received an invalid PER key slot");
			}
			eligible = make_uniq<BoundCaseExpression>(DirectIsNull(DirectColumn(types[slot], bindings[slot])),
			                                          DirectConstantBool(false), std::move(eligible));
		}
		state.eligible = ColumnBinding(state.state_index, expressions.size());
		expressions.push_back(std::move(eligible));
	}
	for (auto &column : extra) {
		state.extra.emplace_back(state.state_index, expressions.size());
		expressions.push_back(std::move(column));
	}
	auto projection = make_uniq<LogicalProjection>(state.state_index, std::move(expressions));
	projection->children.push_back(std::move(input));
	state.plan = std::move(projection);
	return state;
}

DirectBoundValidation DirectValidateBounds(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                           vector<DirectBoundSpec> &bounds, vector<DirectNotNullSpec> &not_null,
                                           const ColumnBinding *eligible, const vector<ColumnBinding> &keys,
                                           const vector<LogicalType> &key_types) {
	DirectBoundValidation result;
	result.min_slots.assign(bounds.size(), DConstants::INVALID_INDEX);
	result.max_slots.assign(bounds.size(), DConstants::INVALID_INDEX);
	if (bounds.empty() && not_null.empty()) {
		result.plan = std::move(input);
		return result;
	}
	// One window computes, per bound, the invalid-value count over all rows and the group extrema.
	result.window_index = optimizer.binder.GenerateTableIndex();
	auto window = make_uniq<LogicalWindow>(result.window_index);
	vector<idx_t> invalid_slots;
	for (idx_t i = 0; i < bounds.size(); i++) {
		auto &bound = bounds[i];
		auto value = [&]() {
			return BoundCastExpression::AddCastToType(optimizer.context, bound.value->Copy(), LogicalType::DOUBLE);
		};
		unique_ptr<Expression> is_invalid =
		    DirectIsNull(bound.is_column ? DirectColumn(bound.column_type, bound.column) : value());
		if (!bound.is_column || bound.column_type == LogicalType::FLOAT || bound.column_type == LogicalType::DOUBLE) {
			is_invalid = make_uniq<BoundCaseExpression>(std::move(is_invalid), DirectConstantBool(true),
			                                            optimizer.BindScalarFunction("isnan", value()));
		}
		invalid_slots.push_back(window->expressions.size());
		window->expressions.push_back(DirectWindowMatchingCount(std::move(is_invalid)));
		auto add_extremum = [&](const char *name) {
			unique_ptr<Expression> reduced = value();
			bool masked = eligible && !bound.all_group_rows;
			if (masked) {
				reduced =
				    make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, *eligible), std::move(reduced),
				                                   make_uniq<BoundConstantExpression>(Value(LogicalType::DOUBLE)));
			}
			auto extremum = DirectWindowExtremum(optimizer, name, std::move(reduced));
			if (masked) {
				extremum->partitions.push_back(DirectColumn(LogicalType::BOOLEAN, *eligible));
			}
			for (idx_t k = 0; k < keys.size(); k++) {
				extremum->partitions.push_back(DirectColumn(key_types[k], keys[k]));
			}
			auto slot = window->expressions.size();
			window->expressions.push_back(std::move(extremum));
			return slot;
		};
		if (DirectIsUpperBound(bound.comparison)) {
			result.min_slots[i] = add_extremum("min");
		}
		if (DirectIsLowerBound(bound.comparison)) {
			result.max_slots[i] = add_extremum("max");
		}
	}
	vector<idx_t> null_slots;
	for (auto &check : not_null) {
		unique_ptr<Expression> is_invalid = DirectIsNull(check.value->Copy());
		if (check.reject_nan) {
			auto as_double =
			    BoundCastExpression::AddCastToType(optimizer.context, check.value->Copy(), LogicalType::DOUBLE);
			is_invalid = make_uniq<BoundCaseExpression>(std::move(is_invalid), DirectConstantBool(true),
			                                            optimizer.BindScalarFunction("isnan", std::move(as_double)));
		}
		null_slots.push_back(window->expressions.size());
		window->expressions.push_back(DirectWindowMatchingCount(std::move(is_invalid)));
	}
	window->children.push_back(std::move(input));
	unique_ptr<LogicalOperator> ready = std::move(window);

	// An equality bound must not vary within its group: count rows whose group MIN and MAX differ.
	idx_t equality_window_index = DConstants::INVALID_INDEX;
	vector<idx_t> equality_slots(bounds.size(), DConstants::INVALID_INDEX);
	unique_ptr<LogicalWindow> equality_window;
	for (idx_t i = 0; i < bounds.size(); i++) {
		if (bounds[i].comparison != ExpressionType::COMPARE_EQUAL) {
			continue;
		}
		if (!equality_window) {
			equality_window_index = optimizer.binder.GenerateTableIndex();
			equality_window = make_uniq<LogicalWindow>(equality_window_index);
		}
		unique_ptr<Expression> varies = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_NOTEQUAL,
		    DirectColumn(LogicalType::DOUBLE, ColumnBinding(result.window_index, result.min_slots[i])),
		    DirectColumn(LogicalType::DOUBLE, ColumnBinding(result.window_index, result.max_slots[i])));
		if (eligible) {
			varies = make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, *eligible), std::move(varies),
			                                        DirectConstantBool(false));
		}
		equality_slots[i] = equality_window->expressions.size();
		equality_window->expressions.push_back(DirectWindowMatchingCount(std::move(varies)));
	}
	if (equality_window) {
		equality_window->children.push_back(std::move(ready));
		ready = std::move(equality_window);
	}

	// One predicate raises an error when any clause fails. Which failing clause is named is not specified.
	auto none = [&](idx_t window_index, idx_t slot) {
		return make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_EQUAL, DirectColumn(LogicalType::BIGINT, ColumnBinding(window_index, slot)),
		    make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
	};
	vector<unique_ptr<Expression>> clause_checks;
	for (idx_t i = 0; i < bounds.size(); i++) {
		unique_ptr<Expression> equality_valid = DirectConstantBool(true);
		if (bounds[i].comparison == ExpressionType::COMPARE_EQUAL) {
			equality_valid = make_uniq<BoundCaseExpression>(
			    none(equality_window_index, equality_slots[i]), DirectConstantBool(true),
			    DirectErrorPredicate(optimizer, "DECIDE source-valued equality bound varies within a group"));
		}
		clause_checks.push_back(
		    make_uniq<BoundCaseExpression>(none(result.window_index, invalid_slots[i]), std::move(equality_valid),
		                                   DirectErrorPredicate(optimizer, bounds[i].invalid_message)));
	}
	for (idx_t i = 0; i < not_null.size(); i++) {
		clause_checks.push_back(make_uniq<BoundCaseExpression>(none(result.window_index, null_slots[i]),
		                                                       DirectConstantBool(true),
		                                                       DirectErrorPredicate(optimizer, not_null[i].message)));
	}
	unique_ptr<Expression> valid = DirectConstantBool(true);
	for (idx_t i = clause_checks.size(); i > 0; i--) {
		valid = make_uniq<BoundCaseExpression>(std::move(clause_checks[i - 1]), std::move(valid),
		                                       DirectConstantBool(false));
	}
	auto guard = make_uniq<LogicalFilter>(std::move(valid));
	guard->children.push_back(std::move(ready));
	result.plan = std::move(guard);
	return result;
}

} // namespace direct_s1
} // namespace duckdb
