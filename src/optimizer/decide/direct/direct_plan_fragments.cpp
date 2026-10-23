#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

unique_ptr<BoundWindowExpression> DirectWindowMatchingCount(const ExpressionBuilder &calc,
                                                            unique_ptr<Expression> predicate) {
	// Hand-built: a window is an operator-specific node, which the builder (scalar expressions) has no operation for.
	auto count = make_uniq<BoundWindowExpression>(
	    ExpressionType::WINDOW_AGGREGATE, LogicalType::BIGINT,
	    make_uniq<AggregateFunction>(CountFun::GetFunctions().GetFunctionByOffset(0)), nullptr);
	count->start = WindowBoundary::UNBOUNDED_PRECEDING;
	count->end = WindowBoundary::UNBOUNDED_FOLLOWING;
	count->children.push_back(calc.Out(calc.If(calc.Wrap(std::move(predicate)), calc.Constant(Value::INTEGER(1)),
	                                           calc.Constant(Value(LogicalType::INTEGER)))));
	return count;
}

DirectPipelineBreaker DirectValidationBreaker(Binder &binder, unique_ptr<LogicalOperator> input,
                                              unique_ptr<Expression> valid) {
	ExpressionBuilder calc(binder.context);
	input->ResolveOperatorTypes();
	auto bindings = input->GetColumnBindings();
	if (bindings.size() != input->types.size() || valid->return_type != LogicalType::BOOLEAN) {
		throw InternalException("Direct solve validation breaker received an invalid input");
	}
	DirectPipelineBreaker result;
	auto projection_index = binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> expressions;
	for (idx_t i = 0; i < bindings.size(); i++) {
		expressions.push_back(calc.Out(calc.Col(input->types[i], bindings[i])));
		result.input_bindings.emplace_back(projection_index, i);
	}
	expressions.push_back(std::move(valid));
	auto projection = make_uniq<LogicalProjection>(projection_index, std::move(expressions));
	projection->children.push_back(std::move(input));
	auto window_index = binder.GenerateTableIndex();
	auto window = make_uniq<LogicalWindow>(window_index);
	window->expressions.push_back(DirectWindowMatchingCount(
	    calc, calc.Out(calc.Col(LogicalType::BOOLEAN, ColumnBinding(projection_index, bindings.size())))));
	window->children.push_back(std::move(projection));
	result.breaker = ColumnBinding(window_index, 0);
	result.plan = std::move(window);
	return result;
}

unique_ptr<Expression> DirectScaledTerm(Optimizer &optimizer, unique_ptr<Expression> value, int sign, bool scaled,
                                        double scale, bool divides) {
	ExpressionBuilder calc(optimizer.context);
	auto term = calc.Cast(calc.Wrap(std::move(value)), LogicalType::DOUBLE);
	if (sign < 0) {
		term = calc.Neg(term);
	}
	if (scaled) {
		term = divides ? calc.Div(term, calc.Num(scale)) : calc.Mul(term, calc.Num(scale));
	}
	return calc.Out(term);
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

unique_ptr<Expression> DirectActiveWhen(const ExpressionBuilder &calc, const Expression *when) {
	return calc.Out(when ? calc.If(calc.Wrap(when->Copy()), calc.Bool(true), calc.Bool(false)) : calc.Bool(true));
}

unique_ptr<Expression> DirectActiveRowsCheck(Optimizer &optimizer, ColumnBinding active_count, const string &clause) {
	ExpressionBuilder calc(optimizer.context);
	auto has_active_rows = calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, calc.Col(LogicalType::BIGINT, active_count),
	                                calc.Constant(Value::BIGINT(0)));
	return DirectRaiseUnless(calc, calc.Out(has_active_rows),
	                         DirectErrorPredicate(optimizer, DecideEmptyRowSetMessage("aggregate", clause)));
}

unique_ptr<LogicalOperator> DirectGuardEmptyAggregate(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                                      ColumnBinding eligible, const string &clause) {
	ExpressionBuilder calc(optimizer.context);
	auto active_window_index = optimizer.binder.GenerateTableIndex();
	auto active_count = DirectWindowMatchingCount(calc, calc.Out(calc.Col(LogicalType::BOOLEAN, eligible)));
	auto active_window = make_uniq<LogicalWindow>(active_window_index);
	active_window->expressions.push_back(std::move(active_count));
	active_window->children.push_back(std::move(input));
	auto active_guard =
	    make_uniq<LogicalFilter>(DirectActiveRowsCheck(optimizer, ColumnBinding(active_window_index, 0), clause));
	active_guard->children.push_back(std::move(active_window));
	return std::move(active_guard);
}

unique_ptr<BoundWindowExpression> DirectWindowExtremum(Optimizer &optimizer, const char *name,
                                                       unique_ptr<Expression> value) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(value));
	auto bound = optimizer.BindAggregateFunction(name, std::move(children));
	auto &aggregate = bound->Cast<BoundAggregateExpression>();
	// Hand-built: a window is an operator-specific node, which the builder (scalar expressions) has no operation for.
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
	ExpressionBuilder calc(binder.context);
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
		expressions.push_back(calc.Out(calc.Col(types[i], bindings[i])));
		state.input_bindings[i] = ColumnBinding(state.state_index, i);
	}
	if (scoped) {
		auto eligible = calc.Wrap(DirectActiveWhen(calc, when));
		for (auto slot : key_slots) {
			D_ASSERT(slot < bindings.size());
			eligible = calc.If(calc.IsNull(calc.Col(types[slot], bindings[slot])), calc.Bool(false), eligible);
		}
		state.eligible = ColumnBinding(state.state_index, expressions.size());
		expressions.push_back(calc.Out(eligible));
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
	ExpressionBuilder calc(optimizer.context);
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
	// Per bound, the window slot that counts its NULL rows (or NULL and NaN rows, for a bound with one message), and
	// the slot that counts its NaN rows when NaN has a message of its own.
	vector<idx_t> invalid_slots;
	vector<idx_t> nan_slots(bounds.size(), DConstants::INVALID_INDEX);
	for (idx_t i = 0; i < bounds.size(); i++) {
		auto &bound = bounds[i];
		auto value = [&]() {
			return calc.Cast(calc.Of(*bound.value), LogicalType::DOUBLE);
		};
		auto is_invalid = calc.IsNull(bound.is_column ? calc.Col(bound.column_type, bound.column) : value());
		// A bound with its own NULL message counts NULL rows here and NaN rows apart; any other counts both together.
		// Only a bound that is exactly one named column has its own message, so any other is computed.
		bool own_messages = !bound.null_message.empty();
		D_ASSERT(own_messages || !bound.is_column);
		if (!own_messages) {
			is_invalid = calc.If(is_invalid, calc.Bool(true), calc.Fn("isnan", value()));
		}
		invalid_slots.push_back(window->expressions.size());
		window->expressions.push_back(DirectWindowMatchingCount(calc, calc.Out(is_invalid)));
		if (!bound.nan_message.empty()) {
			nan_slots[i] = window->expressions.size();
			window->expressions.push_back(DirectWindowMatchingCount(calc, calc.Out(calc.Fn("isnan", value()))));
		}
		auto add_extremum = [&](const char *name) {
			auto reduced = value();
			bool masked = eligible && !bound.all_group_rows;
			if (masked) {
				reduced = calc.If(calc.Col(LogicalType::BOOLEAN, *eligible), reduced,
				                  calc.Constant(Value(LogicalType::DOUBLE)));
			}
			auto extremum = DirectWindowExtremum(optimizer, name, calc.Out(reduced));
			if (masked) {
				extremum->partitions.push_back(calc.Out(calc.Col(LogicalType::BOOLEAN, *eligible)));
			}
			for (idx_t k = 0; k < keys.size(); k++) {
				extremum->partitions.push_back(calc.Out(calc.Col(key_types[k], keys[k])));
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
	vector<idx_t> not_null_nan_slots(not_null.size(), DConstants::INVALID_INDEX);
	for (idx_t i = 0; i < not_null.size(); i++) {
		auto &check = not_null[i];
		auto is_invalid = calc.IsNull(calc.Of(*check.value));
		auto is_nan = [&]() {
			return calc.Fn("isnan", calc.Cast(calc.Of(*check.value), LogicalType::DOUBLE));
		};
		if (!check.nan_message.empty()) {
			not_null_nan_slots[i] = window->expressions.size();
			window->expressions.push_back(DirectWindowMatchingCount(calc, calc.Out(is_nan())));
		} else if (check.reject_nan) {
			is_invalid = calc.If(is_invalid, calc.Bool(true), is_nan());
		}
		null_slots.push_back(window->expressions.size());
		window->expressions.push_back(DirectWindowMatchingCount(calc, calc.Out(is_invalid)));
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
		auto varies = calc.Cmp(ExpressionType::COMPARE_NOTEQUAL,
		                       calc.Col(LogicalType::DOUBLE, ColumnBinding(result.window_index, result.min_slots[i])),
		                       calc.Col(LogicalType::DOUBLE, ColumnBinding(result.window_index, result.max_slots[i])));
		if (eligible) {
			varies = calc.If(calc.Col(LogicalType::BOOLEAN, *eligible), varies, calc.Bool(false));
		}
		equality_slots[i] = equality_window->expressions.size();
		equality_window->expressions.push_back(DirectWindowMatchingCount(calc, calc.Out(varies)));
	}
	if (equality_window) {
		equality_window->children.push_back(std::move(ready));
		ready = std::move(equality_window);
	}

	// One predicate raises an error when any clause fails. Which failing clause is named is not specified.
	auto none = [&](idx_t window_index, idx_t slot) {
		return calc.Out(calc.Cmp(ExpressionType::COMPARE_EQUAL,
		                         calc.Col(LogicalType::BIGINT, ColumnBinding(window_index, slot)),
		                         calc.Constant(Value::BIGINT(0))));
	};
	vector<unique_ptr<Expression>> clause_checks;
	for (idx_t i = 0; i < bounds.size(); i++) {
		// A bound is checked for NULL first, then for NaN, then (an equality) for a value that varies in its group.
		const auto &null_message = bounds[i].null_message.empty() ? bounds[i].invalid_message : bounds[i].null_message;
		clause_checks.push_back(DirectRaiseUnless(calc, none(result.window_index, invalid_slots[i]),
		                                          DirectErrorPredicate(optimizer, null_message)));
		if (nan_slots[i] != DConstants::INVALID_INDEX) {
			clause_checks.push_back(DirectRaiseUnless(calc, none(result.window_index, nan_slots[i]),
			                                          DirectErrorPredicate(optimizer, bounds[i].nan_message)));
		}
		if (bounds[i].comparison == ExpressionType::COMPARE_EQUAL) {
			clause_checks.push_back(DirectRaiseUnless(
			    calc, none(equality_window_index, equality_slots[i]),
			    DirectErrorPredicate(optimizer, "DECIDE source-valued equality bound varies within a group")));
		}
	}
	for (idx_t i = 0; i < not_null.size(); i++) {
		clause_checks.push_back(DirectRaiseUnless(calc, none(result.window_index, null_slots[i]),
		                                          DirectErrorPredicate(optimizer, not_null[i].message)));
		if (not_null_nan_slots[i] != DConstants::INVALID_INDEX) {
			clause_checks.push_back(DirectRaiseUnless(calc, none(result.window_index, not_null_nan_slots[i]),
			                                          DirectErrorPredicate(optimizer, not_null[i].nan_message)));
		}
	}
	auto guard = make_uniq<LogicalFilter>(DirectAllHold(calc, std::move(clause_checks)));
	guard->children.push_back(std::move(ready));
	result.plan = std::move(guard);
	return result;
}

} // namespace duckdb
