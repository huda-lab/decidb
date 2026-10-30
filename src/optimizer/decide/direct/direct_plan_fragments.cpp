#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"

#include "duckdb/common/decide_value_messages.hpp"
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
#include "duckdb/planner/operator/logical_aggregate.hpp"
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

unique_ptr<LogicalAggregate> DirectAggregateOver(Optimizer &optimizer, unique_ptr<LogicalOperator> child,
                                                 const vector<ExprPtr> &groups,
                                                 vector<unique_ptr<Expression>> aggregates) {
	ExpressionBuilder calc(optimizer.context);
	auto &binder = optimizer.binder;
	auto aggregate =
	    make_uniq<LogicalAggregate>(binder.GenerateTableIndex(), binder.GenerateTableIndex(), std::move(aggregates));
	for (auto &group : groups) {
		aggregate->groups.push_back(calc.Out(group));
	}
	aggregate->children.push_back(std::move(child));
	return aggregate;
}

unique_ptr<LogicalOperator> DirectAllRowsValid(Optimizer &optimizer, unique_ptr<LogicalOperator> rows,
                                               unique_ptr<Expression> valid) {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(valid));
	vector<unique_ptr<Expression>> aggregates;
	aggregates.push_back(optimizer.BindAggregateFunction("bool_and", std::move(arguments)));
	return DirectAggregateOver(optimizer, std::move(rows), {}, std::move(aggregates));
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
	return kept ? calc.If(kept, value, calc.Constant(Value(value->return_type))) : value;
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

unique_ptr<BoundWindowExpression> DirectWindowOfAggregate(const Expression &function, unique_ptr<Expression> argument,
                                                          unique_ptr<Expression> reads) {
	auto &aggregate = function.Cast<BoundAggregateExpression>();
	// Hand-built, as in DirectWindowExtremum.
	auto result = make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_AGGREGATE, aggregate.return_type,
	                                               make_uniq<AggregateFunction>(aggregate.function),
	                                               aggregate.bind_info ? aggregate.bind_info->Copy() : nullptr);
	if (argument) {
		result->children.push_back(std::move(argument));
	}
	result->filter_expr = std::move(reads);
	result->start = WindowBoundary::UNBOUNDED_PRECEDING;
	result->end = WindowBoundary::UNBOUNDED_FOLLOWING;
	return result;
}

unique_ptr<Expression> DirectAggregateOf(const Expression &function, unique_ptr<Expression> argument,
                                         unique_ptr<Expression> reads) {
	auto result = function.Copy();
	auto &aggregate = result->Cast<BoundAggregateExpression>();
	D_ASSERT(aggregate.children.empty() && !aggregate.filter);
	if (argument) {
		aggregate.children.push_back(std::move(argument));
	}
	aggregate.filter = std::move(reads);
	return result;
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

bool DirectProveSharedScope(const DirectIR &ir, const vector<const DirectIRConstraint *> &clauses,
                            DirectSharedScope &scope) {
	D_ASSERT(!clauses.empty());
	auto &first = *clauses[0];
	bool ok;
	auto first_when = first.Membership(ok);
	if (!ok) {
		return false;
	}
	for (auto clause : clauses) {
		auto when = clause->Membership(ok);
		if (!ok || !DirectSameKey(clause->PlainSum()->by, first.PlainSum()->by) ||
		    clause->per_class != first.per_class || !DirectSameKey(clause->classes, first.classes) ||
		    static_cast<bool>(when) != static_cast<bool>(first_when) ||
		    (when && !Expression::Equals(*when, *first_when))) {
			return false;
		}
	}
	if (first_when) {
		if (!DirectIsSourceOnlyPredicate(*first_when, ir.decide_index, ir.source_bindings)) {
			return false;
		}
		scope.when = std::move(first_when);
	}
	auto copy_key = [&](const vector<DirectKeyElement> &from, vector<DirectKeyElement> &to) {
		for (auto &key : from) {
			if (!DirectIsDecisionFreeDeterministic(*key.grouping, ir.decide_index)) {
				return false;
			}
			DirectKeyElement element;
			element.grouping = key.grouping->Copy();
			element.no_group = key.no_group;
			to.push_back(std::move(element));
		}
		return true;
	};
	if (!copy_key(first.PlainSum()->by, scope.keys) || !copy_key(first.classes, scope.classes)) {
		return false;
	}
	for (auto &key : scope.classes) {
		scope.gated = scope.gated || key.no_group;
	}
	return true;
}

unique_ptr<LogicalOperator> DirectAddInstanceFlag(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                                  const Expression *when, const vector<DirectKeyElement> &classes,
                                                  ColumnBinding &flag) {
	ExpressionBuilder calc(optimizer.context);
	auto active = calc.Wrap(DirectActiveWhen(calc, when));
	auto in_class = active;
	for (auto &key : classes) {
		if (key.no_group) {
			auto value = DirectOnKeptRows(calc, active, calc.Of(*key.grouping));
			in_class = calc.If(calc.IsNull(value), calc.Bool(false), in_class);
		}
	}
	auto index = optimizer.binder.GenerateTableIndex();
	auto window = make_uniq<LogicalWindow>(index);
	window->expressions.push_back(DirectWindowExtremum(optimizer, "bool_or", calc.Out(in_class)));
	window->children.push_back(std::move(input));
	flag = ColumnBinding(index, 0);
	return std::move(window);
}

DirectScopeState DirectProjectScope(Binder &binder, unique_ptr<LogicalOperator> input, bool scoped,
                                    const Expression *when, const vector<DirectKeyElement> &keys,
                                    vector<unique_ptr<Expression>> extra, unique_ptr<Expression> instance) {
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
		if (instance) {
			eligible = calc.If(calc.Wrap(std::move(instance)), eligible, calc.Bool(false));
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

ExprPtr DirectRowBoundMessage(const ExpressionBuilder &calc, const DirectRowBound &bound) {
	auto none = calc.Constant(Value(LogicalType::VARCHAR));
	auto message = none;
	auto as_double = calc.Cast(calc.Of(*bound.value), LogicalType::DOUBLE);
	if (bound.coefficient) {
		message = calc.If(calc.Fn("isfinite", as_double), none,
		                  calc.Constant(Value(DecideInvalidValueMessage(DecideValueRole::COEFFICIENT, bound.written))));
	} else if (bound.reject_nan) {
		message = calc.If(calc.Fn("isnan", as_double),
		                  calc.Constant(Value(DecideInvalidValueMessage(DecideValueRole::BOUND, bound.written))), none);
	}
	// A NULL value skips the instance it reaches.
	message = calc.If(calc.IsNull(calc.Of(*bound.value)), none, message);
	if (bound.kept) {
		message = calc.If(calc.Of(*bound.kept), message, none);
	}
	return message;
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
		auto message = DirectRowBoundMessage(calc, bound);
		window->expressions.push_back(DirectWindowExtremum(optimizer, "min", calc.Out(message)));
	}
	window->children.push_back(std::move(input));

	vector<unique_ptr<Expression>> checks;
	for (idx_t i = 0; i < bounds.size(); i++) {
		auto message = calc.Col(LogicalType::VARCHAR, ColumnBinding(window_index, i));
		checks.push_back(DirectRaiseMessage(optimizer, calc, message));
	}
	auto guard = make_uniq<LogicalFilter>(DirectAllHold(calc, std::move(checks)));
	guard->children.push_back(std::move(window));
	return std::move(guard);
}

//===--------------------------------------------------------------------===//
// Group bounds
//===--------------------------------------------------------------------===//

unique_ptr<DecideGroupBound> DirectTightestBound(Optimizer &optimizer, ExpressionType comparison,
                                                 unique_ptr<Expression> rhs, const string &text,
                                                 unique_ptr<Expression> invalid) {
	ExpressionBuilder calc(optimizer.context);
	auto bound = make_uniq<DecideGroupBound>();
	bound->text = text;
	auto type = rhs->return_type;
	auto table_index = optimizer.binder.GenerateTableIndex();
	auto add = [&](const char *name) {
		vector<unique_ptr<Expression>> children;
		children.push_back(rhs->Copy());
		auto function = optimizer.BindAggregateFunction(name, std::move(children));
		function->Cast<BoundAggregateExpression>().children.clear();
		bound->aggregates.push_back({ColumnBinding(table_index, bound->aggregates.size()), std::move(function), 0});
		return bound->aggregates.size() - 1;
	};
	idx_t smallest = DConstants::INVALID_INDEX, largest = DConstants::INVALID_INDEX;
	if (DirectIsUpperBound(comparison)) {
		smallest = add("min");
		bound->upper = calc.Out(calc.Col(type, bound->aggregates[smallest].column));
	}
	if (DirectIsLowerBound(comparison)) {
		largest = add("max");
		bound->lower = calc.Out(calc.Col(type, bound->aggregates[largest].column));
	}
	if (comparison == ExpressionType::COMPARE_EQUAL) {
		bound->low = smallest;
		bound->high = largest;
	}
	bound->inputs.push_back({std::move(rhs), nullptr, std::move(invalid)});
	return bound;
}

bool DirectGroupBoundIsReadable(const DirectIR &ir, const DecideGroupBound &bound) {
	auto readable = ir.source_bindings;
	for (auto &aggregate : bound.aggregates) {
		readable.push_back(aggregate.column);
	}
	auto of_the_row = [&](const unique_ptr<Expression> &expr) {
		return !expr || (DirectIsDecisionFreeDeterministic(*expr, ir.decide_index) &&
		                 DirectReferencesOnlySource(*expr, ir.source_bindings));
	};
	for (auto &input : bound.inputs) {
		if (!of_the_row(input.argument) || !of_the_row(input.filter)) {
			return false;
		}
	}
	for (auto &limit : {&bound.upper, &bound.lower}) {
		if (*limit && !DirectIsSourceOnlyNumericMayRaise(**limit, ir.decide_index, readable)) {
			return false;
		}
	}
	return true;
}

DirectGroupInputLayout DirectGroupLayout(const DecideGroupBound::Input &input, bool scoped) {
	return {scoped || input.filter, input.argument != nullptr, input.invalid != nullptr};
}

vector<DirectGroupInput> DirectGroupInputs(const ExpressionBuilder &calc, const DecideGroupBound &bound,
                                           const ExprPtr &eligible,
                                           const std::function<unique_ptr<Expression>(const Expression &)> &over) {
	vector<DirectGroupInput> inputs;
	for (auto &input : bound.inputs) {
		DirectGroupInput columns;
		columns.reads = eligible;
		if (input.filter) {
			auto own = calc.Wrap(DirectActiveWhen(calc, over(*input.filter).get()));
			columns.reads = eligible ? calc.If(eligible, own, calc.Bool(false)) : own;
		}
		// A reducer reads its argument on the rows it reads alone: nothing is computed on any other.
		if (input.argument) {
			auto argument = calc.Wrap(over(*input.argument));
			columns.argument = DirectOnKeptRows(calc, columns.reads, argument);
		}
		if (input.invalid) {
			columns.invalid = calc.Wrap(over(*input.invalid));
		}
		inputs.push_back(std::move(columns));
	}
	return inputs;
}

DirectGroupValues DirectAddGroupBound(const ExpressionBuilder &calc, const DecideGroupBound &bound,
                                      const vector<DirectGroupInputColumns> &inputs, DirectGroupSink &sink,
                                      const ExprPtr &eligible) {
	vector<ColumnBinding> placeholders;
	vector<unique_ptr<Expression>> results;
	for (auto &aggregate : bound.aggregates) {
		auto &input = bound.inputs[aggregate.input];
		auto &columns = inputs[aggregate.input];
		auto argument = input.argument ? calc.Out(calc.Col(input.argument->return_type, columns.argument)) : nullptr;
		auto reads = columns.reads.table_index != DConstants::INVALID_INDEX
		                 ? calc.Out(calc.Col(LogicalType::BOOLEAN, columns.reads))
		                 : nullptr;
		auto result = sink.Add(*aggregate.function, std::move(argument), std::move(reads));
		placeholders.push_back(aggregate.column);
		results.push_back(calc.Out(calc.Col(aggregate.function->return_type, result)));
	}
	// Outside a group the aggregates read nothing, and nothing computed from them is read.
	auto in_group = [&](ExprPtr value) {
		return eligible ? calc.If(eligible, value, calc.Constant(Value(value->return_type))) : value;
	};
	auto over_results = [&](const Expression &expr) {
		auto copy = expr.Copy();
		DirectReplaceColumns(copy, placeholders, results);
		return in_group(calc.Wrap(std::move(copy)));
	};
	DirectGroupValues values;
	if (bound.upper) {
		values.upper = over_results(*bound.upper);
	}
	if (bound.lower) {
		values.lower = over_results(*bound.lower);
	}
	if (bound.invalid) {
		values.messages.push_back(over_results(*bound.invalid));
	}
	if (bound.low != DConstants::INVALID_INDEX) {
		values.messages.push_back(in_group(
		    DirectBoundDisagreement(calc, bound.text, calc.Of(*results[bound.low]), calc.Of(*results[bound.high]))));
	}
	return values;
}

} // namespace duckdb
