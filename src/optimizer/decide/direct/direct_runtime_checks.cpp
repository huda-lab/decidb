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

string DirectInvalidNumberMessage(DirectNumberKind kind) {
	return kind == DirectNumberKind::OBJECTIVE ? DecideInvalidValueMessage("objective coefficient", "NaN or Infinity")
	                                           : DirectCoefficientInvalidMessage();
}

//! The error for a NaN or infinite number, naming the kind of coefficient the user wrote it in.
static unique_ptr<Expression> DirectInvalidNumberError(Optimizer &optimizer, DirectNumberKind kind) {
	return DirectErrorPredicate(optimizer, DirectInvalidNumberMessage(kind));
}

unique_ptr<Expression> DirectRaiseMessage(Optimizer &optimizer, const ExpressionBuilder &calc, const ExprPtr &message) {
	return DirectRaiseIf(calc, calc.Out(calc.If(calc.IsNull(message), calc.Bool(false), calc.Bool(true))),
	                     DirectErrorPredicate(optimizer, calc.Out(message)));
}

string DirectBoundNaNMessage() {
	return DecideInvalidValueMessage("constraint right-hand side", "NaN");
}

string DirectCoefficientInvalidMessage() {
	return DecideInvalidValueMessage("constraint coefficient", "NaN or Infinity");
}

unique_ptr<Expression> DirectCheckOnKeptRows(const ExpressionBuilder &calc, unique_ptr<Expression> kept,
                                             unique_ptr<Expression> check) {
	return calc.Out(calc.If(calc.Wrap(std::move(kept)), calc.Wrap(std::move(check)), calc.Bool(true)));
}

unique_ptr<Expression> DirectValidNumberPredicate(Optimizer &optimizer, const LogicalType &number_type,
                                                  ColumnBinding number_binding, DirectNumberKind kind) {
	ExpressionBuilder calc(optimizer.context);
	auto number = calc.Col(number_type, number_binding);
	auto finite = calc.If(calc.IsNull(number), calc.Bool(true), calc.Fn("isfinite", number));
	return DirectRaiseUnless(calc, calc.Out(finite), DirectInvalidNumberError(optimizer, kind));
}

unique_ptr<Expression> DirectValidBoundPredicate(Optimizer &optimizer, ColumnBinding bound_binding) {
	ExpressionBuilder calc(optimizer.context);
	auto bound = calc.Col(LogicalType::DOUBLE, bound_binding);
	return DirectRaiseIf(calc, calc.Out(calc.Fn("isnan", bound)),
	                     DirectErrorPredicate(optimizer, DirectBoundNaNMessage()));
}

} // namespace duckdb
