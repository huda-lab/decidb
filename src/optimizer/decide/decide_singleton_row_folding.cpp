//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/optimizer/decide/decide_singleton_row_folding.cpp
//
// A bound on one decision: the values it admits, and singleton-row folding of a bound known before the query runs
// into a column box. See decide_optimizer.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"

#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/decide_solver_gate.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
// A bound on one decision: the values it admits
// ---------------------------------------------------------------------------
//
// `x <= 10`, `2 * x >= -4`, `0.1 * x <= cap` and an easy `min(x) >= K` each bound one decision: the clause admits an
// interval of its values and says nothing else. The interval is read here once, as expressions, and every reader uses
// them: singleton-row folding folds an interval known before the query runs into the column box, stage 08 evaluates a
// data bound's interval per row so the model builder writes it into the column box, and direct solve projects it. So
// the solver path and direct solve cannot disagree about which values a bound admits.
//
// The interval is what DuckDB says. A whole-numbered decision admits `n` exactly when the written comparison, with `n`
// in place of the decision, evaluates true: `0.1 * x <= 0.3` admits 3 (`0.1 * 3` is the DECIMAL 0.3), while
// `x <= 0.3 / 0.1` does not (the DOUBLE quotient is 2.9999999999999996). The quotient `r / a` in doubles is within one
// of the answer, and the written comparison, typed as the binder typed it, picks among its neighbours. No copy of
// DuckDB's DECIMAL or DOUBLE arithmetic is written here, so none can drift from it (parity by construction).
//
// A bound is read by value, not by spelling (D41). The decision side is linear terms of one decision over coefficients
// that hold no decision, as the term splitter reads them for the linear form and direct solve, or an easy
// `factor * min(m)` / `max(m)` whose member `m` is such terms. Canonicalization has moved every shift to the other side
// and merged repeated constant terms, so `x - 1 >= -3`, `2 * (x - 1) >= -6` and `x + x >= -4` arrive as one term.

//! Replaces, in `expr`, each node `is_term` matches by `replacement` of it.
static void ReplaceDecisionTerm(unique_ptr<Expression> &expr, const std::function<bool(const Expression &)> &is_term,
                                const std::function<unique_ptr<Expression>(const Expression &)> &replacement) {
	if (is_term(*expr)) {
		expr = replacement(*expr);
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { ReplaceDecisionTerm(child, is_term, replacement); });
}

//! The interval of values `a * x <comparison> r` admits, with `admits(n)` the written comparison at `x = n`. On a side
//! the clause leaves open the end is infinite; a zero coefficient admits every value or none. A whole-numbered
//! decision checks the whole numbers next to `r / a` while that quotient is below `exact_limit` in magnitude, where
//! each candidate is a value of the decision's type; a REAL one has `exact_limit` 0.
static void AdmittedInterval(const ExpressionBuilder &calc, ExpressionType comparison, double exact_limit,
                             const ExprPtr &a, const ExprPtr &r, const std::function<ExprPtr(const ExprPtr &)> &admits,
                             ExprPtr &lower, ExprPtr &upper) {
	constexpr double INF = std::numeric_limits<double>::infinity();
	auto positive_infinity = calc.Num(INF);
	auto negative_infinity = calc.Num(-INF);
	auto zero = calc.Num(0.0);
	auto v = calc.Div(r, a);
	// The greatest admitted value, for a side bounded above, and the least, for a side bounded below.
	ExprPtr greatest = v;
	ExprPtr least = v;
	if (exact_limit > 0.0) {
		// Below the limit the quotient is within one of the answer. Beyond it, or when it is not a number, the
		// rounded quotient is the answer: past 2^52 a DOUBLE holds no fraction to round, and past an INTEGER's range
		// the bound lies beyond every value the decision can take.
		auto exact = calc.Cmp(ExpressionType::COMPARE_LESSTHAN, calc.Fn("abs", v), calc.Num(exact_limit));
		auto one = calc.Num(1.0);
		auto floored = calc.Fn("floor", v);
		auto above_floor = calc.Add(floored, one);
		greatest = calc.If(
		    exact, calc.If(admits(above_floor), above_floor, calc.If(admits(floored), floored, calc.Sub(floored, one))),
		    floored);
		auto ceiled = calc.Fn("ceil", v);
		auto below_ceil = calc.Sub(ceiled, one);
		least = calc.If(exact,
		                calc.If(admits(below_ceil), below_ceil, calc.If(admits(ceiled), ceiled, calc.Add(ceiled, one))),
		                ceiled);
	}
	auto positive = calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, a, zero);
	auto negative = calc.Cmp(ExpressionType::COMPARE_LESSTHAN, a, zero);
	bool at_most =
	    comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO || comparison == ExpressionType::COMPARE_LESSTHAN;
	bool at_least =
	    comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO || comparison == ExpressionType::COMPARE_GREATERTHAN;
	// `at most` bounds x above when the coefficient is positive and below when it is negative; `at least` the opposite;
	// `=` both.
	if (at_most) {
		upper = calc.If(positive, greatest, positive_infinity);
		lower = calc.If(negative, least, negative_infinity);
	} else if (at_least) {
		upper = calc.If(negative, greatest, positive_infinity);
		lower = calc.If(positive, least, negative_infinity);
	} else {
		upper = greatest;
		lower = least;
	}
	auto holds = admits(zero);
	auto zero_coefficient = calc.Cmp(ExpressionType::COMPARE_EQUAL, a, zero);
	upper = calc.If(zero_coefficient, calc.If(holds, positive_infinity, negative_infinity), upper);
	lower = calc.If(zero_coefficient, calc.If(holds, negative_infinity, positive_infinity), lower);
	// A right side the solvers read as infinite, in the direction no value can miss, binds nothing.
	if (at_most || at_least) {
		auto vacuous =
		    at_most ? calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, r, calc.Num(DECIDE_EFFECTIVE_INFINITY))
		            : calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, r, calc.Num(-DECIDE_EFFECTIVE_INFINITY));
		upper = calc.If(vacuous, positive_infinity, upper);
		lower = calc.If(vacuous, negative_infinity, lower);
	}
	// A NULL coefficient or right side skips the instance, which admits every value.
	auto skipped = calc.Any({calc.IsNull(a), calc.IsNull(r)});
	upper = calc.If(skipped, positive_infinity, upper);
	lower = calc.If(skipped, negative_infinity, lower);
}

//! The decision side as the row it bounds. An easy `max(m) <= K` / `min(m) >= K` bounds every member its reducer
//! reads (its `when`, the reducer's filter, chooses them), and the extremum of one member is that member: the clause is
//! read with the member in the reducer's place (`2 * max(c * x) <= K` reads as `2 * (c * x) <= K`), and `filter` is
//! set to the reducer's filter. The reducer stays in the tree, so EXPLAIN and DIAGNOSE read the clause as written.
static unique_ptr<Expression> ReadAsRow(const ExpressionBuilder &calc, const BoundComparisonExpression &comparison,
                                        unique_ptr<Expression> &filter) {
	auto row = calc.Out(calc.Of(*comparison.left));
	if (HasDecideTag(comparison.GetAlias(), MINMAX_EASY_REWRITE_TAG)) {
		ReplaceDecisionTerm(
		    row, [](const Expression &node) { return node.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE; },
		    [&](const Expression &node) {
			    auto &reducer = node.Cast<BoundAggregateExpression>();
			    D_ASSERT(reducer.children.size() == 1);
			    filter = reducer.filter ? reducer.filter->Copy() : nullptr;
			    return calc.Out(calc.Cast(calc.Of(*reducer.children[0]), reducer.return_type));
		    });
	}
	return row;
}

//! Reads `row` as `a * x + b`: linear terms of one decision, whose coefficients add up to `a`, and constant terms,
//! which add up to `b` (null when there are none). The canonicalizer merges constant coefficients (`x + x` is `2 * x`)
//! and moves a clause's shifts to the right side; terms over data (`cap * x + w * x`) stay apart, and a member keeps
//! its own (`max(x + 1)`).
static bool ReadAffine(const ExpressionBuilder &calc, const DecideTermSplitter &splitter, const Expression &row,
                       idx_t &var_idx, ExprPtr &a, ExprPtr &b) {
	vector<DecideSplitTerm> terms;
	splitter.Split(row, 1, terms);
	var_idx = DConstants::INVALID_INDEX;
	for (auto &term : terms) {
		bool constant = term.kind == DecideTermKind::CONSTANT;
		if ((!constant && term.kind != DecideTermKind::LINEAR) || !term.coefficient ||
		    (!constant && var_idx != DConstants::INVALID_INDEX && term.var_a != var_idx)) {
			return false;
		}
		auto value = calc.Mul(calc.Num(term.sign), calc.Cast(calc.Of(*term.coefficient), LogicalType::DOUBLE));
		auto &sum = constant ? b : a;
		sum = sum ? calc.Add(sum, value) : value;
		if (!constant) {
			var_idx = term.var_a;
		}
	}
	return var_idx != DConstants::INVALID_INDEX;
}

//! Fills the fields a bound known before the query runs carries (D41): everything folds to a number, which a column,
//! a subquery or a parameter does not.
static void FoldKnownBound(ClientContext &context, const ExpressionBuilder &calc, ExpressionType type, const ExprPtr &a,
                           const ExprPtr &r, DecideSingletonRow &out) {
	double a_value, r_value;
	out.folds = TryEvaluateFoldableDouble(context, *calc.Out(a), a_value, FOLD_CATCH_ALL) && a_value != 0.0 &&
	            !std::isnan(a_value) && TryEvaluateFoldableDouble(context, *calc.Out(r), r_value, FOLD_CATCH_ALL) &&
	            !std::isnan(r_value) &&
	            TryEvaluateFoldableDouble(context, *out.lower, out.lower_value, FOLD_CATCH_ALL) &&
	            TryEvaluateFoldableDouble(context, *out.upper, out.upper_value, FOLD_CATCH_ALL);
	if (!out.folds) {
		return;
	}
	bool strict = type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_GREATERTHAN;
	bool at_most = type == ExpressionType::COMPARE_LESSTHANOREQUALTO || type == ExpressionType::COMPARE_LESSTHAN;
	out.sense = type == ExpressionType::COMPARE_EQUAL ? '=' : ((at_most == (a_value > 0.0)) ? '<' : '>');
	double quotient = r_value / a_value;
	out.written_k = strict ? DecideStrictBound(out.sense == '>', quotient) : quotient;
	// A side that admits no value (`x >= +inf`) is no box a backend reads: the clause stays a row the model reports
	// infeasible without a solve (SolverModel::ProvesInfeasible).
	constexpr double INF = std::numeric_limits<double>::infinity();
	out.folds = (out.sense == '<' || out.lower_value != INF) && (out.sense == '>' || out.upper_value != -INF);
}

bool DecideReadSingletonRow(ClientContext &context, const LogicalDecide &decide,
                                const BoundComparisonExpression &comparison, DecideSingletonRow &out) {
	auto type = comparison.type;
	if (type != ExpressionType::COMPARE_LESSTHAN && type != ExpressionType::COMPARE_GREATERTHAN &&
	    type != ExpressionType::COMPARE_LESSTHANOREQUALTO && type != ExpressionType::COMPARE_GREATERTHANOREQUALTO &&
	    type != ExpressionType::COMPARE_EQUAL) {
		return false;
	}
	ExpressionBuilder calc(context);
	DecideTermSplitter splitter(context, decide.decide_index, decide.decide_variables);
	unique_ptr<Expression> member_filter;
	auto row = ReadAsRow(calc, comparison, member_filter);
	ExprPtr a, b;
	if (!ReadAffine(calc, splitter, *row, out.var_idx, a, b) || out.var_idx >= decide.decide_variables.size()) {
		return false;
	}
	auto type_id = decide.decide_variables[out.var_idx]->return_type.id();
	bool whole = type_id == LogicalTypeId::INTEGER || type_id == LogicalTypeId::BIGINT;
	if (!whole && type_id != LogicalTypeId::DOUBLE) {
		return false;
	}
	// A strict bound steps onto the whole numbers (`x < 10` admits 9). The binder refuses one on a REAL, which has no
	// such step (ValidateDecideIntegralComparisonOperands).
	D_ASSERT(whole || (type != ExpressionType::COMPARE_LESSTHAN && type != ExpressionType::COMPARE_GREATERTHAN));
	// The right side moved onto `a * x`.
	auto r = calc.Cast(calc.Of(*comparison.right), LogicalType::DOUBLE);
	if (b) {
		r = calc.Sub(r, b);
	}
	// The check is a comparison of its own, bound as the binder binds one: both sides cast to the type it compares in.
	// A rebuilt clause (`x <= -sum(w) + 10`) is a model of the constraint and may compare a BIGINT with a DECIMAL. Any
	// reference that names the decision takes `n`, whatever type the binder gave it there.
	auto input_type = BoundComparisonExpression::BindComparison(context, comparison.left->return_type,
	                                                            comparison.right->return_type, type);
	auto admits = [&](const ExprPtr &n) {
		auto left = row->Copy();
		ReplaceDecisionTerm(
		    left,
		    [&](const Expression &node) {
			    return node.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
			           splitter.FindVariable(node) == out.var_idx;
		    },
		    [&](const Expression &node) { return calc.Out(calc.Cast(n, node.return_type)); });
		// The check reads the row and `n` only: a decision left in it would read a source column at stage 08.
		D_ASSERT(!BoundExpressionReferencesDecide(*left, decide.decide_index));
		return calc.Cmp(type, calc.Cast(calc.Wrap(std::move(left)), input_type),
		                calc.Cast(calc.Of(*comparison.right), input_type));
	};
	ExprPtr lower, upper;
	double exact_limit = !whole                              ? 0.0
	                     : type_id == LogicalTypeId::INTEGER ? double(NumericLimits<int32_t>::Maximum())
	                                                         : DECIDE_MAX_FRACTIONAL_MAGNITUDE;
	AdmittedInterval(calc, type, exact_limit, a, r, admits, lower, upper);
	out.lower = calc.Out(lower);
	out.upper = calc.Out(upper);
	FoldKnownBound(context, calc, type, a, r, out);
	out.constant_on_read_rows = out.folds;
	if (member_filter) {
		// A row the reducer's filter leaves out is no member and admits every value; its member is never read. The
		// interval then depends on the row, so it folds into the box only if the filter itself is known.
		auto member = calc.Of(*member_filter);
		lower = calc.If(member, lower, calc.Num(-std::numeric_limits<double>::infinity()));
		upper = calc.If(member, upper, calc.Num(std::numeric_limits<double>::infinity()));
		out.folds = false;
		DecideSingletonRow filtered;
		filtered.lower = calc.Out(lower);
		filtered.upper = calc.Out(upper);
		FoldKnownBound(context, calc, type, a, r, filtered);
		if (filtered.folds) {
			out.folds = true;
			out.sense = filtered.sense;
			out.lower_value = filtered.lower_value;
			out.upper_value = filtered.upper_value;
			out.written_k = filtered.written_k;
		}
		out.lower = std::move(filtered.lower);
		out.upper = std::move(filtered.upper);
	}
	return true;
}

void DecideWrittenBox::Add(char sense, double k) {
	DecideFoldSingletonRow(box, sense, k);
}

void DecideWrittenBox::AddLowered(char sense, double k) {
	if (sense != '<' && k < 0.0) {
		DecideFoldSingletonRow(box, '>', k);
	}
}

bool DecideWrittenBox::ContradictsDomain(bool is_bool) const {
	double floor = box.EffectiveLower();
	// A BOOLEAN's floor is 0 whatever lower bound was written (a lower bound below 0 changes nothing), so an upper
	// bound below 0 contradicts it even when the written lower bound is negative (`x = -1`).
	return (box.upper < 0.0 && (floor >= 0.0 || is_bool)) || (is_bool && floor > 1.0);
}

void DecideOptimizer::FoldSingletonRowsInExpression(Expression &expr, LogicalDecide &decide) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		// `when c:` -- a conditional per-row constraint must NOT contribute to a global
		// bound: `when c: x <= 0` does not mean `x <= 0` everywhere. This is why the pass keeps
		// its own descent rather than using ForEachConstraintLeaf -- it asks the shared
		// predicate what a wrapper is, then deliberately answers differently.
		if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			break;
		}
		for (auto &child : conj.children) {
			FoldSingletonRowsInExpression(*child, decide);
		}
		break;
	}

	case ExpressionClass::BOUND_COMPARISON: {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		idx_t source_clause_id = DConstants::INVALID_INDEX;
		TryParseSourceClauseTag(comp.GetAlias(), source_clause_id);
		idx_t removal_group_id = DConstants::INVALID_INDEX;
		TryParseRemovalGroupTag(comp.GetAlias(), removal_group_id);

		DecideSingletonRow bound;
		if (!DecideReadSingletonRow(optimizer.context, decide, comp, bound) || !bound.folds) {
			break;
		}
		// The comparison stays in the tree so EXPLAIN keeps rendering what the user
		// wrote; the tag is what stops it also becoming a model row. The box takes the
		// admitted ends (`x <= 42.9999999` admits 42 for an INT), tightening
		// unconditionally: a BOOLEAN restatement is a harmless no-op against its
		// intrinsic box. A `-inf` floor writes the solver's "no bound", which frees `x`
		// below as `x >= -1e30` does (D41).
		AddDecideTag(comp.alias, SINGLETON_ROW_BOUND_TAG);
		auto &box = decide.singleton_row_boxes[bound.var_idx];
		if (bound.sense != '>') {
			DecideFoldSingletonRow(box, '<', bound.upper_value);
		}
		if (bound.sense != '<') {
			DecideFoldSingletonRow(box, '>', bound.lower_value);
		}
		// A bound that merely restates a BOOLEAN's intrinsic [0,1] box is not a
		// loosenable parameter: the domain is applied to the solver column directly and
		// never synthesized as a constraint, so such a bound only exists because the
		// user wrote it redundantly. A genuine pin (`x <= 0`, `x >= 1`, `x = 1`) is
		// recorded. Only a recorded bound is loosened by a diagnosis, and a clause is one
		// knob per kept row or class in an expanded diagnosis (D28), which one column
		// bound cannot say, so a recorded one is flattened for the repair model too.
		bool is_boolean = bound.var_idx < decide.is_boolean_var.size() && decide.is_boolean_var[bound.var_idx];
		bool restates_domain = is_boolean && bound.lower_value <= 0.0 && bound.upper_value >= 1.0;
		if (!restates_domain) {
			decide.user_singleton_row_bounds.push_back(
			    {bound.var_idx, bound.sense, bound.written_k, source_clause_id, removal_group_id});
			AddDecideTag(comp.alias, SINGLETON_ROW_CLAUSE_ROWS_TAG);
		}
		break;
	}

	case ExpressionClass::BOUND_CONSTANT:
		// Type declarations and rewrite placeholders (`TRUE`) carry no bound.
		break;

	default:
		break;
	}
}

void DecideOptimizer::FoldSingletonRows(LogicalDecide &decide) {
	// Sized here rather than at construction so every auxiliary variable created by the
	// preceding passes is already counted.
	idx_t num_decide_vars = decide.decide_variables.size();
	decide.singleton_row_boxes.assign(num_decide_vars, DecideBoundBox());
	decide.user_singleton_row_bounds.clear();

	// Seed a BOOLEAN's intrinsic ceiling here rather than leaving every variable at
	// 1e30 and repairing it at model-build time. The box travels to stage 06 as
	// `SolverInput::upper_bounds`, and every Big-M derivation reads it through
	// `DecideRowTermRange`, which treats `>= 1e20` as unbounded: a declared BOOL with no
	// written upper bound therefore looked unbounded to all of them and collapsed to the
	// fallback constant. `SUM(x) <> 2` over four BOOLs took M = 1000000 where the same
	// query spelled `x(INT) ... x <= 1` took 7.
	//
	// The ceiling is a property of the declared type, known here, and it is rigid: the
	// elastic engine resets BOOLEAN columns only within [0,1], so no diagnosis opens it.
	// That also lets the `<>` range collapse see a BOOL's upper side, which it could not
	// before. Folding only ever narrows (`std::min`), and a user restatement like
	// `x <= 1` is already treated as a harmless no-op against the intrinsic box, so
	// seeding it changes nothing about how a written bound is recorded.
	for (idx_t i = 0; i < num_decide_vars; i++) {
		if (i < decide.is_boolean_var.size() && decide.is_boolean_var[i]) {
			decide.singleton_row_boxes[i].upper = 1.0;
			// The floor is just as rigid. A written lower bound only ever raises it (`max`), so `x >= -3`
			// leaves the box at [0,1] and a bound that cannot hold (`x >= 2`, `x <= -1`) inverts it, which
			// the solve reports as infeasible. Without this the box would widen to [-3,1], and x would
			// take a value that is not a Boolean.
			DecideFoldSingletonRow(decide.singleton_row_boxes[i], '>', 0.0);
		}
	}
	// A helper that holds a value of either sign (`sum(x - 5)`, `power(x, 2) - 10 * x`): no default floor.
	for (auto var : decide.signed_helper_vars) {
		decide.singleton_row_boxes[var].lower_written = true;
	}

	if (decide.decide_constraints) {
		FoldSingletonRowsInExpression(*decide.decide_constraints, decide);
	}
}

} // namespace duckdb
