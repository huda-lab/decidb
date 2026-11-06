#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/decide_number_warning.hpp"

#include <cstdio>

namespace duckdb {

DecideNumberWarningState &DecideNumberWarningState::Of(ClientContext &context) {
	return *context.registered_state->GetOrCreate<DecideNumberWarningState>(DECIDE_NUMBER_WARNING_STATE_KEY);
}

void DecideNumberWarningState::Say() {
	if (!said.exchange(true)) {
		fprintf(stderr, "%s\n", DECIDE_NUMBER_RANGE_WARNING);
	}
}

void DecideNumberWarningState::QueryBegin(ClientContext &context) {
	said = false;
}

namespace {

void DecideWarnNumbersFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DecideNumberWarningState::Of(state.GetContext()).Say();
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<bool>(result)[0] = true;
}

} // namespace

ScalarFunction InternalDecideWarnNumbersFun::GetFunction() {
	ScalarFunction fun("__internal_decide_warn_numbers", {}, LogicalType::BOOLEAN, DecideWarnNumbersFunction);
	// It has a side effect, so no plan may fold it or share it between rows.
	fun.stability = FunctionStability::VOLATILE;
	return fun;
}

} // namespace duckdb
