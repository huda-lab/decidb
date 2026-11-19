#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"

#include <limits>

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"

namespace duckdb {

unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, const string &message) {
	ExpressionBuilder calc(optimizer.context);
	// `error()` raises its text as an Invalid Input Error with no place.
	return calc.Out(calc.Cast(calc.Fn("error", calc.Constant(Value(message))), LogicalType::BOOLEAN));
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

unique_ptr<Expression> DirectRaiseTooLarge(Optimizer &optimizer, const ExprPtr &number, const DecideClauseRef &clause) {
	ExpressionBuilder calc(optimizer.context);
	return calc.Out(DecideNumberCheck(calc, number, clause));
}

unique_ptr<Expression> DirectCheckNumber(Optimizer &optimizer, ColumnBinding number, const DecideNumberNames &names,
                                         const vector<DirectSignedValue> &values, const DirectFactor &factor) {
	if (DirectIsFiniteWhereKept(optimizer, values, factor)) {
		return nullptr;
	}
	ExpressionBuilder calc(optimizer.context);
	return calc.Out(DecideNumberCheck(calc, calc.Col(LogicalType::DOUBLE, number), names));
}

unique_ptr<Expression> DirectValidBoundPredicate(Optimizer &optimizer, ColumnBinding bound_binding,
                                                 const string &written, optional_idx place) {
	ExpressionBuilder calc(optimizer.context);
	auto bound = calc.Col(LogicalType::DOUBLE, bound_binding);
	return DirectRaiseIf(calc, calc.Out(calc.Fn("isnan", bound)),
	                     calc.Out(calc.RaiseInvalidValue(DecideValueRole::BOUND, written, place)));
}

} // namespace duckdb
