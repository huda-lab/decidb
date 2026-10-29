#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
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

//===--------------------------------------------------------------------===//
// DECIDE semantics every aggregate rule shares
//===--------------------------------------------------------------------===//

unique_ptr<Expression> DirectActiveWhen(const ExpressionBuilder &calc, const Expression *when) {
	return calc.Out(when ? calc.If(calc.Wrap(when->Copy()), calc.Bool(true), calc.Bool(false)) : calc.Bool(true));
}

ExprPtr DirectOnKeptRows(const ExpressionBuilder &calc, const ExprPtr &kept, const ExprPtr &value) {
	return calc.If(kept, value, calc.Constant(Value(value->return_type)));
}

ExprPtr DirectBoundDisagreement(const ExpressionBuilder &calc, const string &text, const ExprPtr &low,
                                const ExprPtr &high) {
	// `text` is the bound as written (RenderDecideSource quotes a subquery from its source fragment).
	auto format = StringUtil::Replace("`" + text + "`", "%", "%%") +
	              " takes more than one value here (%g and %g) — an `=` bound cannot hold two different values "
	              "at once, so this DECIDE query is infeasible.";
	auto message = calc.Fn("printf", {calc.Constant(Value(format)), calc.Cast(low, LogicalType::DOUBLE),
	                                  calc.Cast(high, LogicalType::DOUBLE)});
	return calc.If(calc.Cmp(ExpressionType::COMPARE_NOTEQUAL, low, high), message,
	               calc.Constant(Value(LogicalType::VARCHAR)));
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
                                    const Expression *when, const vector<DirectKeyElement> &keys,
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
		auto active = calc.Wrap(DirectActiveWhen(calc, when));
		auto eligible = active;
		vector<ExprPtr> key_values;
		for (auto &key : keys) {
			auto value = calc.Of(*key.grouping);
			key_values.push_back(when ? DirectOnKeptRows(calc, active, value) : value);
			if (key.no_group) {
				eligible = calc.If(calc.IsNull(key_values.back()), calc.Bool(false), eligible);
			}
		}
		state.eligible = ColumnBinding(state.state_index, expressions.size());
		expressions.push_back(calc.Out(eligible));
		for (auto &value : key_values) {
			state.keys.emplace_back(state.state_index, expressions.size());
			state.key_types.push_back(value->return_type);
			expressions.push_back(calc.Out(value));
		}
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

unique_ptr<LogicalOperator> DirectCheckRowBounds(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                                 const vector<DirectRowBound> &bounds) {
	if (bounds.empty()) {
		return input;
	}
	ExpressionBuilder calc(optimizer.context);
	// One window takes, per bound, the least message among the rows it is bad on: NULL on a row the bound is valid.
	auto window_index = optimizer.binder.GenerateTableIndex();
	auto window = make_uniq<LogicalWindow>(window_index);
	for (auto &bound : bounds) {
		auto none = calc.Constant(Value(LogicalType::VARCHAR));
		auto message = none;
		auto as_double = calc.Cast(calc.Of(*bound.value), LogicalType::DOUBLE);
		if (bound.coefficient) {
			message =
			    calc.If(calc.Fn("isfinite", as_double), none, calc.Constant(Value(DirectCoefficientInvalidMessage())));
		} else if (bound.reject_nan) {
			message = calc.If(calc.Fn("isnan", as_double), calc.Constant(Value(DirectBoundNaNMessage())), none);
		}
		// A NULL value skips the instance it reaches.
		message = calc.If(calc.IsNull(calc.Of(*bound.value)), none, message);
		if (bound.kept) {
			message = calc.If(calc.Of(*bound.kept), message, none);
		}
		window->expressions.push_back(DirectWindowExtremum(optimizer, "min", calc.Out(message)));
	}
	window->children.push_back(std::move(input));

	vector<unique_ptr<Expression>> checks;
	for (idx_t i = 0; i < bounds.size(); i++) {
		auto message = calc.Col(LogicalType::VARCHAR, ColumnBinding(window_index, i));
		checks.push_back(DirectRaiseIf(calc, calc.Out(calc.If(calc.IsNull(message), calc.Bool(false), calc.Bool(true))),
		                               DirectErrorPredicate(optimizer, calc.Out(message))));
	}
	auto guard = make_uniq<LogicalFilter>(DirectAllHold(calc, std::move(checks)));
	guard->children.push_back(std::move(window));
	return std::move(guard);
}

} // namespace duckdb
