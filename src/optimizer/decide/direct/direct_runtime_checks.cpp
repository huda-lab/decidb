#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"

#include <limits>

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"

namespace duckdb {

unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, unique_ptr<Expression> message) {
	ExpressionBuilder calc(optimizer.context);
	// Raised as the located error its message carries (DecideLocatedMessage), `error()` where it carries no place.
	return calc.Out(
	    calc.Cast(calc.RaiseInvalidValue(DecideValueRole::MESSAGE, calc.Wrap(std::move(message)), optional_idx()),
	              LogicalType::BOOLEAN));
}

unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, const string &message) {
	ExpressionBuilder calc(optimizer.context);
	return DirectErrorPredicate(optimizer, calc.Out(calc.Constant(Value(message))));
}

unique_ptr<Expression> DirectRaiseUnless(const ExpressionBuilder &calc, unique_ptr<Expression> ok,
                                         unique_ptr<Expression> error) {
	return calc.Out(calc.If(calc.Wrap(std::move(ok)), calc.Bool(true), calc.Wrap(std::move(error))));
}

unique_ptr<Expression> DirectRaiseIf(const ExpressionBuilder &calc, unique_ptr<Expression> bad,
                                     unique_ptr<Expression> error) {
	return calc.Out(calc.If(calc.Wrap(std::move(bad)), calc.Wrap(std::move(error)), calc.Bool(true)));
}

unique_ptr<Expression> DirectWarnIf(const ExpressionBuilder &calc, unique_ptr<Expression> unstable) {
	return calc.Out(calc.If(calc.Wrap(std::move(unstable)),
	                        calc.Fn("__internal_decide_warn_numbers", vector<ExprPtr> {}), calc.Bool(true)));
}

ExprPtr DirectBelowNormalRange(const ExpressionBuilder &calc, const ExprPtr &value) {
	return calc.Cmp(ExpressionType::COMPARE_LESSTHAN, calc.Fn("abs", value),
	                calc.Num(std::numeric_limits<double>::min()));
}

unique_ptr<Expression> DirectAllHold(const ExpressionBuilder &calc, vector<unique_ptr<Expression>> checks) {
	auto chain = calc.Bool(true);
	for (idx_t i = checks.size(); i > 0; i--) {
		chain = calc.If(calc.Wrap(std::move(checks[i - 1])), chain, calc.Bool(false));
	}
	return calc.Out(chain);
}

unique_ptr<Expression> DirectInfeasibleErrorPredicate(Optimizer &optimizer) {
	return DirectErrorPredicate(optimizer, DecideInfeasibleMessage());
}

unique_ptr<Expression> DirectUnboundedErrorPredicate(Optimizer &optimizer) {
	return DirectErrorPredicate(optimizer, DecideUnboundedMessage());
}

void DirectNameMerge(DirectNumberNames &names, const DecideSourceText &source, const DirectNumberNames &other) {
	if (!other.named) {
		return;
	}
	if (!names.named) {
		names.named = true;
		names.constant = other.constant;
		names.text = other.text;
		names.place = other.place;
		return;
	}
	names.constant = names.constant && other.constant;
	names.place = DecideCoveringLocation(source, names.place, other.place);
	string covering;
	int ignored;
	if (source.CutExpr(names.place, covering, ignored)) {
		names.text = covering;
	}
}

void DirectNameValue(DirectNumberNames &names, const DecideSourceText &source, const Expression &value,
                     const string &text, optional_idx held_at) {
	auto &unguarded = DecideUnguarded(value);
	DirectNumberNames one;
	one.place = DecideSpanLocation(source, held_at.IsValid() ? held_at : unguarded.GetQueryLocation());
	one.constant = unguarded.IsFoldable();
	// A number the engine made for itself is no value of the user's.
	one.named = !one.constant || one.place.IsValid();
	one.text = text;
	int ignored;
	if (held_at.IsValid() && one.place.IsValid()) {
		source.CutExpr(one.place, one.text, ignored);
	}
	DirectNameMerge(names, source, one);
}

//! What a number that is not finite is called: the expression of the query it stands for, and its clause when it has
//! none.
static ExprPtr NotFiniteMessage(const ExpressionBuilder &calc, const DirectNumberNames &names) {
	if (names.named) {
		return calc.RaiseInvalidValue(names.constant ? DecideValueRole::CONSTANT : DecideValueRole::COEFFICIENT,
		                              names.text, names.place);
	}
	// A message, not a raise: it is raised where the plan's check chain says (DirectErrorPredicate).
	return calc.Constant(Value(DecideLocatedMessage(
	    DecideInvalidValueMessage(DecideValueRole::TOO_LARGE, names.clause.Noun()), names.clause.place)));
}

ExprPtr DirectNumberMessage(const ExpressionBuilder &calc, const ExprPtr &number, const DirectNumberNames &names) {
	auto none = calc.Constant(Value(LogicalType::VARCHAR));
	return calc.If(calc.IsNull(number), none,
	               calc.If(calc.Fn("isfinite", number), none, NotFiniteMessage(calc, names)));
}

ExprPtr DirectNumberMessage(const ExpressionBuilder &calc, const ExprPtr &number, const DecideClauseRef &clause) {
	DirectNumberNames names;
	names.clause = clause;
	return DirectNumberMessage(calc, number, names);
}

unique_ptr<Expression> DirectRaiseMessage(Optimizer &optimizer, const ExpressionBuilder &calc, const ExprPtr &message) {
	return DirectRaiseUnless(calc, calc.Out(calc.IsNull(message)), DirectErrorPredicate(optimizer, calc.Out(message)));
}

//! The check of a number: true when it is finite, and otherwise the error `bad` names unless the number is NULL. The
//! gate is one BOOLEAN per row, and `bad` is built on the rows that fail alone, as a CASE evaluates a branch only where
//! it is chosen.
static unique_ptr<Expression> RaiseUnlessFinite(Optimizer &optimizer, const ExpressionBuilder &calc,
                                                const ExprPtr &number, const ExprPtr &bad) {
	auto raise = calc.Wrap(
	    DirectRaiseUnless(calc, calc.Out(calc.IsNull(number)), DirectErrorPredicate(optimizer, calc.Out(bad))));
	return calc.Out(calc.If(calc.Fn("isfinite", number), calc.Bool(true), raise));
}

unique_ptr<Expression> DirectRaiseTooLarge(Optimizer &optimizer, const ExprPtr &number, const DecideClauseRef &clause) {
	DirectNumberNames names;
	names.clause = clause;
	return DirectRaiseNotFinite(optimizer, number, names);
}

unique_ptr<Expression> DirectRaiseNotFinite(Optimizer &optimizer, const ExprPtr &number,
                                            const DirectNumberNames &names) {
	ExpressionBuilder calc(optimizer.context);
	return RaiseUnlessFinite(optimizer, calc, number, NotFiniteMessage(calc, names));
}

unique_ptr<Expression> DirectCheckNumber(Optimizer &optimizer, ColumnBinding number, const DirectNumberNames &names,
                                         const vector<DirectSignedValue> &values, const DirectFactor &factor) {
	if (DirectIsFiniteWhereKept(optimizer, values, factor)) {
		return nullptr;
	}
	ExpressionBuilder calc(optimizer.context);
	auto column = calc.Col(LogicalType::DOUBLE, number);
	return RaiseUnlessFinite(optimizer, calc, column, NotFiniteMessage(calc, names));
}

unique_ptr<Expression> DirectValidBoundPredicate(Optimizer &optimizer, ColumnBinding bound_binding,
                                                 const string &written, optional_idx place) {
	ExpressionBuilder calc(optimizer.context);
	auto bound = calc.Col(LogicalType::DOUBLE, bound_binding);
	auto raise = calc.Cast(calc.RaiseInvalidValue(DecideValueRole::BOUND, written, place), LogicalType::BOOLEAN);
	return DirectRaiseIf(calc, calc.Out(calc.Fn("isnan", bound)), calc.Out(raise));
}

} // namespace duckdb
