#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"

#include <algorithm>

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"

namespace duckdb {

unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, unique_ptr<Expression> message) {
	ExpressionBuilder calc(optimizer.context);
	return calc.Out(calc.Cast(calc.Fn("error", calc.Wrap(std::move(message))), LogicalType::BOOLEAN));
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

unique_ptr<Expression> DirectAllHold(const ExpressionBuilder &calc, vector<unique_ptr<Expression>> checks) {
	auto chain = calc.Bool(true);
	for (idx_t i = checks.size(); i > 0; i--) {
		chain = calc.If(calc.Wrap(std::move(checks[i - 1])), chain, calc.Bool(false));
	}
	return calc.Out(chain);
}

unique_ptr<Expression> DirectInfeasibleErrorPredicate(Optimizer &optimizer) {
	return DirectErrorPredicate(optimizer, "DECIDE optimization is infeasible. Prefix the query with DIAGNOSE to see "
	                                       "which clause to change.");
}

unique_ptr<Expression> DirectUnboundedErrorPredicate(Optimizer &optimizer) {
	return DirectErrorPredicate(optimizer, "DECIDE optimization is unbounded. Prefix the query with DIAGNOSE to see "
	                                       "which decision needs a bound.");
}

ExprPtr DirectNumberMessage(const ExpressionBuilder &calc, const ExprPtr &number, const DirectNumberNames &names,
                            const std::function<ExprPtr(const Expression &)> &into) {
	auto message = calc.Constant(Value(DecideInvalidValueMessage(DecideValueRole::TOO_LARGE, names.clause)));
	// Built from the last input back, so the first that is not finite is the one named. A NULL input is no answer.
	for (idx_t i = names.inputs.size(); i > 0; i--) {
		auto input = calc.Cast(into(*names.inputs[i - 1]), LogicalType::DOUBLE);
		auto not_finite = calc.Cmp(ExpressionType::COMPARE_EQUAL, calc.Fn("isfinite", input), calc.Bool(false));
		auto named = DecideInvalidValueMessage(DecideValueRole::COEFFICIENT, names.texts[i - 1]);
		message = calc.If(not_finite, calc.Constant(Value(named)), message);
	}
	auto none = calc.Constant(Value(LogicalType::VARCHAR));
	return calc.If(calc.IsNull(number), none, calc.If(calc.Fn("isfinite", number), none, message));
}

ExprPtr DirectNumberMessage(const ExpressionBuilder &calc, const ExprPtr &number, const string &clause) {
	DirectNumberNames names;
	names.clause = clause;
	return DirectNumberMessage(calc, number, names, nullptr);
}

unique_ptr<Expression> DirectRaiseMessage(Optimizer &optimizer, const ExpressionBuilder &calc, const ExprPtr &message) {
	return DirectRaiseIf(calc, calc.Out(calc.If(calc.IsNull(message), calc.Bool(false), calc.Bool(true))),
	                     DirectErrorPredicate(optimizer, calc.Out(message)));
}

unique_ptr<Expression> DirectCheckOnKeptRows(const ExpressionBuilder &calc, unique_ptr<Expression> kept,
                                             unique_ptr<Expression> check) {
	return calc.Out(calc.If(calc.Wrap(std::move(kept)), calc.Wrap(std::move(check)), calc.Bool(true)));
}

unique_ptr<Expression> DirectValidBoundPredicate(Optimizer &optimizer, ColumnBinding bound_binding,
                                                 const string &written) {
	ExpressionBuilder calc(optimizer.context);
	auto bound = calc.Col(LogicalType::DOUBLE, bound_binding);
	return DirectRaiseIf(calc, calc.Out(calc.Fn("isnan", bound)),
	                     DirectErrorPredicate(optimizer, DecideInvalidValueMessage(DecideValueRole::BOUND, written)));
}

} // namespace duckdb
