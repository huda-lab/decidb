#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"

#include <algorithm>
#include <cmath>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"

namespace duckdb {

static bool DirectProveValues(const DirectIR &ir, const vector<DecideSplitTerm> &terms,
                              vector<DirectSignedValue> &out) {
	for (auto &term : terms) {
		if (!term.coefficient || !DirectProveValue(ir, *term.coefficient)) {
			return false;
		}
		DirectSignedValue value;
		value.sign = term.sign;
		value.expr = term.coefficient->Copy();
		out.push_back(std::move(value));
	}
	return true;
}

DirectBoundClause DirectBoundClause::Copy() const {
	DirectBoundClause result;
	result.decision = decision;
	result.comparison = comparison;
	for (auto &value : coefficient) {
		result.coefficient.push_back({value.sign, value.expr->Copy()});
	}
	result.rhs = rhs->Copy();
	if (when) {
		result.when = when->Copy();
	}
	return result;
}

bool DirectReadBounds(const DirectIR &ir, ClientContext &context, DirectBoundReading &reading) {
	idx_t decision_count = ir.decisions.size();
	// The box as the solver folds it: the raw values it reads (after the strict shift, before any rounding or mapping
	// to infinity), combined by the one function the solver's own folding pass calls.
	vector<DecideBoundBox> boxes(decision_count);
	for (auto &ir_node : ir.constraints) {
		if (ir_node.aggregate) {
			continue;
		}
		idx_t decision = ir_node.lhs[0].terms[0].var_a;
		auto domain = ir.decisions[decision].domain;
		auto &rhs = *ir_node.rhs;
		auto comparison = ir_node.comparison;
		if (!DirectIsSourceOnlyValue(rhs, ir.decide_index, ir.source_bindings)) {
			return false;
		}
		if (ir_node.scope.when &&
		    !DirectIsSourceOnlyPredicate(*ir_node.scope.when, ir.decide_index, ir.source_bindings)) {
			return false;
		}
		if (ir_node.variable_bound) {
			double k;
			if (!TryEvaluateFoldableDouble(context, rhs, k, DIRECT_CONSTANT | FOLD_ALLOW_BOOLEAN)) {
				return false;
			}
			bool whole = DirectIsWholeNumbered(domain);
			bool strict =
			    comparison == ExpressionType::COMPARE_LESSTHAN || comparison == ExpressionType::COMPARE_GREATERTHAN;
			if (strict && !whole) {
				return false;
			}
			bool lower = DirectIsLowerBound(comparison);
			bool upper = DirectIsUpperBound(comparison);
			DecideFoldSingletonRow(boxes[decision], lower && upper ? '=' : (lower ? '>' : '<'),
			                       strict ? DecideStrictBound(lower, k) : k);
			continue;
		}
		DirectBoundClause clause;
		clause.decision = decision;
		clause.comparison = comparison;
		if (!DirectProveValues(ir, ir_node.lhs[0].terms, clause.coefficient)) {
			return false;
		}
		clause.rhs = ir_node.rhs->Copy();
		if (ir_node.scope.when) {
			clause.when = ir_node.scope.when->Copy();
		}
		if (comparison == ExpressionType::COMPARE_EQUAL) {
			// An equality is a lower and an upper bound that may not agree: a fractional value for a whole-numbered
			// decision then leaves no row.
			auto upper = clause.Copy();
			upper.comparison = ExpressionType::COMPARE_LESSTHANOREQUALTO;
			clause.comparison = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
			reading.clauses.push_back(std::move(clause));
			reading.clauses.push_back(std::move(upper));
		} else {
			reading.clauses.push_back(std::move(clause));
		}
	}
	for (idx_t decision = 0; decision < decision_count; decision++) {
		// The solver rejects, before it reads any row, a box that contradicts the variable's built-in domain
		// (DecideBoxContradictsDomain). It says so on an empty source too, so the query is left to the solver, which
		// reports it, rather than proved here and answered with no rows.
		if (DecideBoxContradictsDomain(boxes[decision], ir.decisions[decision].domain == DirectDomain::BOOL)) {
			return false;
		}
	}
	reading.boxes.resize(decision_count);
	for (idx_t decision = 0; decision < decision_count; decision++) {
		bool boolean = ir.decisions[decision].domain == DirectDomain::BOOL;
		// A written lower bound replaces the default of 0, except that a BOOL never leaves {0, 1}. A whole-numbered
		// decision rounds each bound inward, and a bound of magnitude 1e20 or more (a written -1e30 included) is no
		// bound.
		bool whole = DirectIsWholeNumbered(ir.decisions[decision].domain);
		double lower = boxes[decision].lower;
		double upper = boxes[decision].upper;
		if (whole) {
			lower = std::ceil(lower);
			upper = std::floor(upper);
		}
		lower = DecideIsOpenBelow(lower) ? NEGATIVE_INFINITY : lower;
		upper = DecideIsOpenAbove(upper) ? POSITIVE_INFINITY : upper;
		reading.boxes[decision].lower = boxes[decision].lower_written ? (boolean ? std::max(0.0, lower) : lower) : 0.0;
		reading.boxes[decision].upper = boolean ? std::min(1.0, upper) : upper;
	}
	return true;
}

void DirectClauseBounds(const ExpressionBuilder &calc, ExpressionType comparison, bool whole,
                        const DirectBoundColumns &columns, ExprPtr &lower, ExprPtr &upper) {
	auto a = calc.Col(LogicalType::DOUBLE, columns.coefficient);
	auto r = calc.Col(LogicalType::DOUBLE, columns.rhs);
	auto zero = calc.Num(0.0);
	auto positive_infinity = calc.Num(POSITIVE_INFINITY);
	auto negative_infinity = calc.Num(NEGATIVE_INFINITY);
	bool at_most =
	    comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO || comparison == ExpressionType::COMPARE_LESSTHAN;

	auto feasible = [&](const ExprPtr &k) {
		return calc.Cmp(comparison, calc.Mul(a, k), r);
	};
	auto v = calc.Div(r, a);
	ExprPtr upper_candidate = v;
	ExprPtr lower_candidate = v;
	if (whole) {
		auto huge = calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, calc.Fn("abs", v),
		                     calc.Num(DECIDE_MAX_FRACTIONAL_MAGNITUDE));
		auto floored = calc.Fn("floor", v);
		auto up_one = calc.Add(floored, calc.Num(1.0));
		auto down_one = calc.Sub(floored, calc.Num(1.0));
		upper_candidate =
		    calc.If(huge, floored, calc.If(feasible(up_one), up_one, calc.If(feasible(floored), floored, down_one)));
		auto ceiled = calc.Fn("ceil", v);
		auto ceil_up = calc.Add(ceiled, calc.Num(1.0));
		auto ceil_down = calc.Sub(ceiled, calc.Num(1.0));
		lower_candidate =
		    calc.If(huge, ceiled, calc.If(feasible(ceil_down), ceil_down, calc.If(feasible(ceiled), ceiled, ceil_up)));
	}
	auto positive = calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, a, zero);
	auto negative = calc.Cmp(ExpressionType::COMPARE_LESSTHAN, a, zero);
	// `at most` clauses bound x above when the coefficient is positive and below when it is negative; `at least`
	// clauses do the opposite.
	auto upper_by_sign = at_most ? calc.If(positive, upper_candidate, positive_infinity)
	                             : calc.If(negative, upper_candidate, positive_infinity);
	auto lower_by_sign = at_most ? calc.If(negative, lower_candidate, negative_infinity)
	                             : calc.If(positive, lower_candidate, negative_infinity);
	// A zero coefficient says `0 comparison rhs`: it holds for every x, or for none.
	auto holds = calc.Cmp(comparison, zero, r);
	auto zero_coefficient = calc.Cmp(ExpressionType::COMPARE_EQUAL, a, zero);
	auto row_upper = calc.If(zero_coefficient, positive_infinity, upper_by_sign);
	auto row_lower = calc.If(zero_coefficient, calc.If(holds, negative_infinity, positive_infinity), lower_by_sign);
	// A right side the solver reads as infinite binds nothing.
	auto vacuous = at_most
	                   ? calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, r, calc.Num(DECIDE_EFFECTIVE_INFINITY))
	                   : calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, r, calc.Num(-DECIDE_EFFECTIVE_INFINITY));
	upper = calc.If(vacuous, positive_infinity, row_upper);
	lower = calc.If(vacuous, negative_infinity, row_lower);
	if (columns.has_when) {
		auto active = calc.Col(LogicalType::BOOLEAN, columns.when);
		upper = calc.If(active, upper, positive_infinity);
		lower = calc.If(active, lower, negative_infinity);
	}
}

void DirectRowBox(const ExpressionBuilder &calc, double static_lower, double static_upper,
                  const vector<ColumnBinding> &clause_lowers, const vector<ColumnBinding> &clause_uppers,
                  ExprPtr &lower, ExprPtr &upper) {
	vector<ExprPtr> lowers {calc.Num(static_lower)};
	vector<ExprPtr> uppers {calc.Num(static_upper)};
	for (auto &binding : clause_lowers) {
		lowers.push_back(calc.Col(LogicalType::DOUBLE, binding));
	}
	for (auto &binding : clause_uppers) {
		uppers.push_back(calc.Col(LogicalType::DOUBLE, binding));
	}
	lower = calc.Max(lowers);
	upper = calc.Min(uppers);
}

} // namespace duckdb
