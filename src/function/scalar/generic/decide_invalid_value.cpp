#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"

namespace duckdb {

namespace {

//! The arguments are the role of the value (a DecideValueRole) and the value as the user wrote it. Where the value
//! stands in the query is the call's own location (Expression::query_location, as a cast carries its own): invalid for
//! a value the query text holds no place for. It raises the error of DecideInvalidValueError, at the value, as the
//! solver path does; a check calls it only on a row whose value is NaN or infinite (`CASE WHEN <refused> THEN raise`),
//! so it returns NULL, of the BOOLEAN a check is.
void DecideInvalidValueFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, true);
	if (args.size() == 0) {
		return;
	}
	// Any row of the chunk is one the check raised on; the role is a constant of the plan.
	auto role = DecideValueRole(args.data[0].GetValue(0).GetValue<int32_t>());
	auto place = state.expr.GetQueryLocation();
	for (idx_t row = 0; row < args.size(); row++) {
		auto written = args.data[1].GetValue(row);
		if (written.IsNull()) {
			continue;
		}
		throw DecideInvalidValueError(role, written.ToString(), place);
	}
}

} // namespace

ScalarFunction InternalDecideInvalidValueFun::GetFunction() {
	ScalarFunction fun("__internal_decide_invalid_value", {LogicalType::INTEGER, LogicalType::VARCHAR},
	                   LogicalType::BOOLEAN, DecideInvalidValueFunction);
	// The value written may be NULL.
	fun.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	// A call with constant arguments must not fold away: it stands in a branch no row may reach.
	fun.stability = FunctionStability::VOLATILE;
	BaseScalarFunction::SetReturnsError(fun);
	return fun;
}

} // namespace duckdb
