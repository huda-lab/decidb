#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"

#include <cmath>

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
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
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

ColumnBinding DirectProjectionStage::Add(unique_ptr<Expression> expression) {
	ColumnBinding binding(index, expressions.size());
	expressions.push_back(std::move(expression));
	return binding;
}

ColumnBinding DirectProjectionStage::Add(const ExprPtr &expression) {
	return Add(expression->Copy());
}

static unique_ptr<Expression> ColumnOf(const LogicalType &type, ColumnBinding binding) {
	return make_uniq<BoundColumnRefExpression>(type, binding);
}

vector<ColumnBinding> DirectProjectionStage::Forward(const vector<ColumnBinding> &columns,
                                                     const vector<LogicalType> &types) {
	vector<ColumnBinding> forwarded;
	for (idx_t i = 0; i < columns.size(); i++) {
		forwarded.push_back(Add(ColumnOf(types[i], columns[i])));
	}
	return forwarded;
}

DirectCarried DirectProjectionStage::Forward(const DirectCarried &from, const vector<LogicalType> &source_types,
                                             bool scoped) {
	DirectCarried to;
	to.source = Forward(from.source, source_types);
	if (scoped) {
		to.eligible = Add(ColumnOf(LogicalType::BOOLEAN, from.eligible));
	}
	to.keys = Forward(from.keys, from.key.types);
	to.key = from.key;
	return to;
}

unique_ptr<LogicalOperator> DirectProjectionStage::Finish(unique_ptr<LogicalOperator> child) {
	auto projection = make_uniq<LogicalProjection>(index, std::move(expressions));
	projection->children.push_back(std::move(child));
	return std::move(projection);
}

unique_ptr<Expression> DirectSumOf(Optimizer &optimizer, const vector<DirectSignedValue> &values,
                                   const std::function<unique_ptr<Expression>(const Expression &)> &over, int sign) {
	ExpressionBuilder calc(optimizer.context);
	unique_ptr<Expression> total;
	for (auto &value : values) {
		auto term = DirectScaledTerm(optimizer, over ? over(*value.expr) : value.expr->Copy(), value.sign * sign, false,
		                             1.0, false);
		total = total ? calc.Out(calc.Add(calc.Wrap(std::move(total)), calc.Wrap(std::move(term)))) : std::move(term);
	}
	return total;
}

DirectQuadraticColumns DirectLayoutQuadraticPart(Optimizer &optimizer, const ExpressionBuilder &calc,
                                                 DirectProjectionStage &p1, const DirectQuadraticPart &part,
                                                 idx_t decision_count, const vector<ColumnBinding> &source_bindings,
                                                 const std::function<unique_ptr<Expression>(const Expression &)> &over,
                                                 vector<unique_ptr<Expression>> &checks, int constant_sign) {
	DirectQuadraticColumns columns;
	// A value is computed and checked only on the rows the part reads.
	ExprPtr kept;
	if (part.kept) {
		columns.has_mask = true;
		kept = calc.Wrap(DirectActiveWhen(calc, over ? over(*part.kept).get() : part.kept.get()));
		columns.mask = p1.Add(calc.Out(kept));
	}
	auto on_kept = [&](const ExprPtr &value) {
		return p1.Add(calc.Out(DirectOnKeptRows(calc, kept, value)));
	};
	auto check_number = [&](ColumnBinding number, const DirectNumberNames &names,
	                        const vector<DirectSignedValue> &values, bool scaled = false, double scale = 1.0,
	                        bool divides = false) {
		if (auto check = DirectCheckNumber(optimizer, number, names, values, source_bindings, p1.index, scaled, scale,
		                                   divides)) {
			checks.push_back(std::move(check));
		}
	};
	for (auto &square : part.squares) {
		DirectQuadraticColumns::Square laid;
		auto &inner = square.inner;
		laid.decision = inner.decision;
		laid.k = on_kept(calc.Wrap(DirectSumOf(optimizer, square.weight, over)));
		check_number(laid.k, square.weight_names, square.weight);
		laid.w = on_kept(calc.Wrap(DirectSumOf(optimizer, inner.linear, over)));
		check_number(laid.w, inner.linear_names, inner.linear);
		if (inner.constant.empty()) {
			laid.t = p1.Add(calc.Out(calc.Num(0.0)));
		} else {
			laid.t = on_kept(calc.Wrap(DirectSumOf(optimizer, inner.constant, over, constant_sign)));
			check_number(laid.t, inner.constant_names, inner.constant);
		}
		columns.squares.push_back(laid);
	}
	// Linear coefficients add up per decision.
	for (idx_t d = 0; d < decision_count; d++) {
		auto values = DirectValuesOn(part.linear, d);
		if (values.empty()) {
			continue;
		}
		auto binding = on_kept(calc.Wrap(DirectSumOf(optimizer, values, over)));
		check_number(binding, part.linear.names[d], values);
		columns.linear_decision.push_back(d);
		columns.linear.push_back(binding);
	}
	// A constant adds nothing to the answer, so like the solver path it is read one term at a time, with the part's
	// factor applied, and never added up; each must still be valid.
	for (auto &term : part.constants) {
		auto value = calc.Wrap(DirectScaledTerm(optimizer, over ? over(*term.value) : term.value->Copy(), term.sign,
		                                        part.linear.scaled, part.linear.scale, part.linear.divides));
		vector<DirectSignedValue> read;
		read.push_back({term.sign, term.value->Copy()});
		check_number(on_kept(value), term.value_names, read, part.linear.scaled, part.linear.scale,
		             part.linear.divides);
	}
	return columns;
}

vector<DirectBoundColumns> DirectLayoutBoundClauses(Optimizer &optimizer, const ExpressionBuilder &calc,
                                                    DirectProjectionStage &p1, const vector<DirectBoundClause> &clauses,
                                                    const vector<ColumnBinding> &source_bindings,
                                                    const std::function<unique_ptr<Expression>(const Expression &)> &over,
                                                    vector<unique_ptr<Expression>> &checks) {
	auto into = [&](const Expression &value) {
		return calc.Wrap(over ? over(value) : value.Copy());
	};
	vector<DirectBoundColumns> result;
	for (auto &clause : clauses) {
		DirectBoundColumns columns;
		ExprPtr kept;
		if (clause.when) {
			kept = calc.Wrap(DirectActiveWhen(calc, over ? over(*clause.when).get() : clause.when.get()));
		}
		auto on_kept = [&](const ExprPtr &value) {
			return DirectOnKeptRows(calc, kept, value);
		};
		auto coefficient = p1.Add(calc.Out(on_kept(calc.Wrap(DirectSumOf(optimizer, clause.coefficient, over)))));
		auto rhs = p1.Add(calc.Out(on_kept(calc.Cast(into(*clause.rhs), LogicalType::DOUBLE))));
		columns.lower = p1.Add(calc.Out(on_kept(into(*clause.lower))));
		columns.upper = p1.Add(calc.Out(on_kept(into(*clause.upper))));
		if (clause.when) {
			columns.has_when = true;
			columns.when = p1.Add(calc.Out(kept));
		}
		if (auto check = DirectCheckNumber(optimizer, coefficient, clause.coefficient_names, clause.coefficient,
		                                   source_bindings, p1.index)) {
			checks.push_back(std::move(check));
		}
		// A right side of an exact type is never NaN.
		if (!std::isfinite(DirectMagnitudeBound(optimizer.context, *clause.rhs))) {
			checks.push_back(DirectValidBoundPredicate(optimizer, rhs, clause.rhs_text));
		}
		result.push_back(columns);
	}
	return result;
}

//! The sum of `terms` added in order, with no 0.0 to start from: 0.0 + x is x except for x = -0.0, and a plan that adds
//! a constant to every row computes it on every row. 0.0 for no terms.
static ExprPtr SumOfTerms(const ExpressionBuilder &calc, const vector<ExprPtr> &terms) {
	if (terms.empty()) {
		return calc.Num(0.0);
	}
	auto total = terms[0];
	for (idx_t i = 1; i < terms.size(); i++) {
		total = calc.Add(total, terms[i]);
	}
	return total;
}

DirectQuadraticSum DirectFoldQuadratic(Optimizer &optimizer, const ExpressionBuilder &calc,
                                       const vector<DirectQuadraticPart> &parts,
                                       const vector<DirectQuadraticColumns> &columns, idx_t decision) {
	D_ASSERT(parts.size() == columns.size());
	vector<ExprPtr> a_terms, b_terms, linear_terms;
	for (idx_t p = 0; p < parts.size(); p++) {
		auto &part = parts[p];
		auto &laid = columns[p];
		vector<ExprPtr> linear_part, a_part, cross_part;
		for (idx_t i = 0; i < laid.linear.size(); i++) {
			if (laid.linear_decision[i] == decision) {
				linear_part.push_back(calc.Col(LogicalType::DOUBLE, laid.linear[i]));
			}
		}
		for (auto &square : laid.squares) {
			if (square.decision != decision) {
				continue;
			}
			auto k = calc.Col(LogicalType::DOUBLE, square.k);
			auto w = calc.Col(LogicalType::DOUBLE, square.w);
			auto t = calc.Col(LogicalType::DOUBLE, square.t);
			a_part.push_back(calc.Mul(k, calc.Mul(w, w)));
			cross_part.push_back(calc.Mul(calc.Num(2.0), calc.Mul(k, calc.Mul(w, t))));
		}
		if (linear_part.empty() && a_part.empty()) {
			continue;
		}
		auto scaled = [&](ExprPtr value) {
			auto result = calc.Wrap(DirectScaledTerm(optimizer, calc.Out(value), part.linear.sign, part.linear.scaled,
			                                         part.linear.scale, part.linear.divides));
			return laid.has_mask ? calc.If(calc.Col(LogicalType::BOOLEAN, laid.mask), result, calc.Num(0.0)) : result;
		};
		vector<ExprPtr> b_part;
		if (!linear_part.empty()) {
			linear_terms.push_back(scaled(SumOfTerms(calc, linear_part)));
			b_part.push_back(linear_terms.back());
		}
		if (!a_part.empty()) {
			a_terms.push_back(scaled(SumOfTerms(calc, a_part)));
			b_part.push_back(scaled(SumOfTerms(calc, cross_part)));
		}
		b_terms.push_back(SumOfTerms(calc, b_part));
	}
	DirectQuadraticSum sum;
	sum.a = SumOfTerms(calc, a_terms);
	sum.b = SumOfTerms(calc, b_terms);
	sum.linear = linear_terms.empty() ? nullptr : SumOfTerms(calc, linear_terms);
	return sum;
}

DirectSquareTarget DirectTurningPoint(const ExpressionBuilder &calc, const ExprPtr &a, const ExprPtr &b, bool whole) {
	// The halves are taken before the division: 2*a passes a DOUBLE for |a| above 9e307, and -0.5*b cannot.
	auto half_down = calc.Mul(calc.Num(-0.5), b);
	DirectSquareTarget result;
	result.target =
	    whole ? calc.Fn("ceil", calc.Div(calc.Sub(half_down, calc.Mul(calc.Num(0.5), a)), a)) : calc.Div(half_down, a);
	auto zero = calc.Num(0.0);
	auto has_square = calc.Cmp(ExpressionType::COMPARE_NOTEQUAL, a, zero);
	auto has_linear = calc.Cmp(ExpressionType::COMPARE_NOTEQUAL, b, zero);
	auto small_linear = calc.If(has_linear, DirectBelowNormalRange(calc, b), calc.Bool(false));
	result.loses_precision =
	    calc.If(has_square, calc.Any({DirectBelowNormalRange(calc, a), small_linear}), calc.Bool(false));
	return result;
}

DirectAggregateStage::DirectAggregateStage(Binder &binder)
    : group_index(binder.GenerateTableIndex()), aggregate_index(binder.GenerateTableIndex()) {
}

ColumnBinding DirectAggregateStage::Group(unique_ptr<Expression> expression) {
	groups.push_back(std::move(expression));
	return ColumnBinding(group_index, groups.size() - 1);
}

ColumnBinding DirectAggregateStage::Add(unique_ptr<Expression> aggregate) {
	aggregates.push_back(std::move(aggregate));
	return ColumnBinding(aggregate_index, aggregates.size() - 1);
}

unique_ptr<LogicalAggregate> DirectAggregateStage::Finish(unique_ptr<LogicalOperator> child) {
	auto aggregate = make_uniq<LogicalAggregate>(group_index, aggregate_index, std::move(aggregates));
	aggregate->groups = std::move(groups);
	aggregate->children.push_back(std::move(child));
	return aggregate;
}

unique_ptr<LogicalAggregate> DirectAggregateOver(Optimizer &optimizer, unique_ptr<LogicalOperator> child,
                                                 const vector<ExprPtr> &groups,
                                                 vector<unique_ptr<Expression>> aggregates) {
	ExpressionBuilder calc(optimizer.context);
	DirectAggregateStage stage(optimizer.binder);
	for (auto &group : groups) {
		stage.Group(calc.Out(group));
	}
	for (auto &aggregate : aggregates) {
		stage.Add(std::move(aggregate));
	}
	return stage.Finish(std::move(child));
}

unique_ptr<LogicalOperator> DirectJoinGroupRow(unique_ptr<LogicalOperator> rows, unique_ptr<LogicalOperator> groups,
                                               vector<unique_ptr<Expression>> row_keys,
                                               vector<unique_ptr<Expression>> group_keys, const vector<bool> &equal) {
	D_ASSERT(row_keys.size() == group_keys.size() && row_keys.size() == equal.size());
	if (row_keys.empty()) {
		return LogicalCrossProduct::Create(std::move(rows), std::move(groups));
	}
	// A SINGLE join, the form DuckDB uses to give each row of a scalar subquery its one matching row: the key is
	// unique in the group plan and every row's group is in it. A SINGLE join has no inverse, so BuildProbeSideOptimizer
	// keeps the group plan the build side, and JOIN_ORDER estimates it as its row child, whereas an inner join is
	// estimated from the aggregates' child/2 cardinalities as far larger than either side and gets flipped to build on
	// the rows.
	auto join = make_uniq<LogicalComparisonJoin>(JoinType::SINGLE);
	for (idx_t i = 0; i < row_keys.size(); i++) {
		JoinCondition on_key;
		on_key.comparison = equal[i] ? ExpressionType::COMPARE_EQUAL : ExpressionType::COMPARE_NOT_DISTINCT_FROM;
		on_key.left = std::move(row_keys[i]);
		on_key.right = std::move(group_keys[i]);
		join->conditions.push_back(std::move(on_key));
	}
	join->children.push_back(std::move(rows));
	join->children.push_back(std::move(groups));
	return std::move(join);
}

DirectJoinedAnswers DirectJoinClassAnswers(Binder &binder, unique_ptr<LogicalOperator> rows,
                                           const vector<ColumnBinding> &row_keys, const DirectClassKey &key,
                                           unique_ptr<LogicalOperator> classes) {
	D_ASSERT(row_keys.size() == key.types.size());
	ExpressionBuilder calc(binder.context);
	auto class_bindings = classes->GetColumnBindings();
	D_ASSERT(class_bindings.size() >= key.types.size());
	vector<unique_ptr<Expression>> on_rows, on_classes;
	for (idx_t i = 0; i < key.types.size(); i++) {
		on_rows.push_back(calc.Out(calc.Col(key.types[i], row_keys[i])));
		on_classes.push_back(calc.Out(calc.Col(key.types[i], class_bindings[i])));
	}
	DirectJoinedAnswers result;
	result.answers.assign(class_bindings.begin() + NumericCast<int64_t>(key.types.size()), class_bindings.end());
	result.plan =
	    DirectJoinGroupRow(std::move(rows), std::move(classes), std::move(on_rows), std::move(on_classes), key.equal);
	return result;
}

unique_ptr<LogicalOperator> DirectAllRowsValid(Optimizer &optimizer, unique_ptr<LogicalOperator> rows,
                                               unique_ptr<Expression> valid) {
	ExpressionBuilder calc(optimizer.context);
	vector<unique_ptr<Expression>> aggregates;
	aggregates.push_back(DirectHashAggregate(optimizer, calc, "bool_and", calc.Wrap(std::move(valid))));
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

static unique_ptr<BoundWindowExpression>
DirectWindowOfAggregate(const Expression &function, unique_ptr<Expression> argument, unique_ptr<Expression> reads) {
	auto &aggregate = function.Cast<BoundAggregateExpression>();
	if (aggregate.bind_info) {
		throw InternalException("DECIDE: a reducer binds data the direct plan does not copy; the binder promises "
		                        "SUM, AVG, MIN, MAX and COUNT bind none");
	}
	// Hand-built, as in DirectWindowExtremum.
	auto result = make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_AGGREGATE, aggregate.return_type,
	                                               make_uniq<AggregateFunction>(aggregate.function), nullptr);
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
	// `function` is a copy of the reducer, whose alias carries its tags; a plan node shows none.
	aggregate.ClearAlias();
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

unique_ptr<Expression> DirectHashAggregate(Optimizer &optimizer, const ExpressionBuilder &calc, const char *name,
                                           const ExprPtr &argument, const ExprPtr &parameter) {
	vector<unique_ptr<Expression>> children;
	if (argument) {
		children.push_back(calc.Out(argument));
	}
	if (parameter) {
		children.push_back(calc.Out(parameter));
	}
	return optimizer.BindAggregateFunction(name, std::move(children));
}

unique_ptr<BoundWindowExpression> DirectRunningSum(Optimizer &optimizer, const ExpressionBuilder &calc,
                                                   const ExprPtr &value, const ExprPtr &order, OrderType direction,
                                                   const vector<ExprPtr> &partitions) {
	auto sum = DirectWindowExtremum(optimizer, "sum", calc.Out(value));
	sum->end = WindowBoundary::CURRENT_ROW_ROWS;
	for (auto &partition : partitions) {
		sum->partitions.push_back(calc.Out(partition));
	}
	sum->orders.emplace_back(direction, OrderByNullType::NULLS_LAST, calc.Out(order));
	return sum;
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
	// A `by` key holds expressions, which can be volatile. A `per` key holds columns and relations only.
	for (auto &key : first.PlainSum()->by) {
		if (!DirectIsDecisionFreeDeterministic(*key.grouping, ir.decide_index)) {
			return false;
		}
		scope.keys.push_back(key.Copy());
	}
	for (auto &key : first.classes) {
		scope.classes.push_back(key.Copy());
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

DirectCarried DirectCarriedOf(const DirectScopeState &state, idx_t source_width) {
	DirectCarried carried;
	carried.source.assign(state.input_bindings.begin(),
	                      state.input_bindings.begin() + NumericCast<int64_t>(source_width));
	carried.eligible = state.eligible;
	carried.keys = state.keys;
	carried.key = state.key;
	return carried;
}

DirectClassKey DirectClassKeyOf(const vector<DirectKeyElement> &keys, bool drops_rows) {
	DirectClassKey key;
	bool has_no_class_element = false;
	for (auto &element : keys) {
		key.types.push_back(element.grouping->return_type);
		key.equal.push_back(element.no_group);
		has_no_class_element = has_no_class_element || element.no_group;
	}
	if (drops_rows && !has_no_class_element) {
		key.types.push_back(LogicalType::UTINYINT);
		key.equal.push_back(true);
	}
	return key;
}

ExprPtr DirectClassPresent(const ExpressionBuilder &calc, const DirectClassKey &key, const vector<ExprPtr> &columns) {
	D_ASSERT(columns.size() == key.types.size());
	ExprPtr present;
	for (idx_t i = 0; i < columns.size(); i++) {
		if (key.equal[i]) {
			auto has_value = calc.Not(calc.IsNull(columns[i]));
			present = present ? calc.And(present, has_value) : has_value;
		}
	}
	return present ? present : calc.Bool(true);
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
		state.key = DirectClassKeyOf(keys, when != nullptr || instance != nullptr);
		if (instance) {
			eligible = calc.If(calc.Wrap(std::move(instance)), eligible, calc.Bool(false));
			// The instance is the second reason a row is in no class, after a `when`, which has put NULL in every
			// column of the key already and a NULL in an element that is no class's on its own.
			for (idx_t i = 0; i < keys.size(); i++) {
				if (keys[i].no_group) {
					key_values[i] = calc.If(eligible, key_values[i], calc.Constant(Value(key_values[i]->return_type)));
				}
			}
		}
		if (key_values.size() < state.key.types.size()) {
			// No element can say a row is in no class, and a `when` or an instance can: the marker does.
			key_values.push_back(
			    calc.If(eligible, calc.Constant(Value::UTINYINT(0)), calc.Constant(Value(LogicalType::UTINYINT))));
		}
		state.eligible = ColumnBinding(state.state_index, expressions.size());
		expressions.push_back(calc.Out(eligible));
		for (auto &value : key_values) {
			state.keys.emplace_back(state.state_index, expressions.size());
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

ExprPtr DirectRowBoundMessage(ClientContext &context, const ExpressionBuilder &calc, const DirectRowBound &bound) {
	auto none = calc.Constant(Value(LogicalType::VARCHAR));
	auto message = none;
	auto as_double = calc.Cast(calc.Of(*bound.value), LogicalType::DOUBLE);
	if (std::isfinite(DirectMagnitudeBound(context, *bound.value))) {
		// A value of an exact type is never NaN or infinite, so the row raises nothing.
		return none;
	}
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

ColumnBinding DirectWindowSink::Add(const Expression &function, unique_ptr<Expression> argument,
                                    unique_ptr<Expression> reads) {
	auto aggregate = DirectWindowOfAggregate(function, std::move(argument), std::move(reads));
	partition(*aggregate);
	window.expressions.push_back(std::move(aggregate));
	return ColumnBinding(window.window_index, window.expressions.size() - 1);
}

ColumnBinding DirectAggregateSink::Add(const Expression &function, unique_ptr<Expression> argument,
                                       unique_ptr<Expression> reads) {
	return aggregate.Add(DirectAggregateOf(function, std::move(argument), std::move(reads)));
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
