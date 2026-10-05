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

static unique_ptr<Expression> DirectVarchar(const string &text) {
	return make_uniq<BoundConstantExpression>(Value(text));
}

static unique_ptr<Expression> DirectBigint(int64_t value) {
	return make_uniq<BoundConstantExpression>(Value::BIGINT(value));
}

static unique_ptr<Expression> DirectIsNull(unique_ptr<Expression> value) {
	auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	is_null->children.push_back(std::move(value));
	return std::move(is_null);
}

//! The NULL message for a score, as an expression over the row it is evaluated on. It spells out what the solver
//! prints for the same row: the NULL column, or the NULL columns, or the score's own text when no column is NULL.
unique_ptr<Expression> DirectValidScorePredicate(Optimizer &optimizer, const LogicalType &score_type,
                                                 ColumnBinding score_binding, const string &score_text) {
	const string advice = "Impute it with COALESCE(), or filter those rows out with a WHERE clause.";
	auto null_message = score_text.empty() ? "DECIDE: a value used in the optimization is NULL. " + advice
	                                       : StringUtil::Format("DECIDE: %s is NULL. %s", score_text, advice);
	auto is_null = DirectIsNull(DirectColumn(score_type, score_binding));
	auto is_finite = optimizer.BindScalarFunction("isfinite", DirectColumn(score_type, score_binding));
	auto finite_or_error = make_uniq<BoundCaseExpression>(
	    std::move(is_finite), DirectConstantBool(true),
	    DirectErrorPredicate(optimizer, "DECIDE objective coefficient contains invalid value (NaN or Infinity). "
	                                    "Common causes:\n"
	                                    "  • Division by zero in the expression\n"
	                                    "  • Arithmetic overflow in calculations\n"
	                                    "  • NULL values that propagated through math operations\n"
	                                    "Check your expressions and input data."));
	return make_uniq<BoundCaseExpression>(std::move(is_null), DirectErrorPredicate(optimizer, null_message),
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

//===--------------------------------------------------------------------===//
// DECIDE semantics every aggregate rule shares
//===--------------------------------------------------------------------===//

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
		unique_ptr<Expression> eligible =
		    when ? make_uniq<BoundCaseExpression>(when->Copy(), DirectConstantBool(true), DirectConstantBool(false))
		         : DirectConstantBool(true);
		for (auto slot : key_slots) {
			if (slot >= bindings.size()) {
				throw InternalException("Direct solve received an invalid PER key slot");
			}
			auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
			is_null->children.push_back(DirectColumn(types[slot], bindings[slot]));
			eligible = make_uniq<BoundCaseExpression>(std::move(is_null), DirectConstantBool(false), std::move(eligible));
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

unique_ptr<LogicalOperator> DirectGuardEmptyAggregate(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                                      ColumnBinding eligible) {
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
	    DirectErrorPredicate(optimizer, "DECIDE empty row set for aggregate in constraint. "
	                                    "An empty aggregate has no well-defined value; check your WHEN clause."));
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
		auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
		is_null->children.push_back(bound.is_column ? DirectColumn(bound.column_type, bound.column) : value());
		unique_ptr<Expression> is_invalid = std::move(is_null);
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
				reduced = make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, *eligible),
				                                         std::move(reduced),
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
		auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
		is_null->children.push_back(check.value->Copy());
		unique_ptr<Expression> is_invalid = std::move(is_null);
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
		return make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL,
		                                            DirectColumn(LogicalType::BIGINT, ColumnBinding(window_index, slot)),
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
		clause_checks.push_back(
		    make_uniq<BoundCaseExpression>(none(result.window_index, null_slots[i]), DirectConstantBool(true),
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

} // namespace duckdb
