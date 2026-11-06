#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"

#include <atomic>
#include <cstdio>

namespace duckdb {

namespace {

//! Whether the current query has said the warning. A direct plan builds no model, so it has no NumberRangeWarning to
//! hold this for it, and its rows run on several threads.
struct DecideNumberWarningState : public ClientContextState {
	std::atomic<bool> said {false};

	void QueryBegin(ClientContext &context) override {
		said = false;
	}
};

void DecideWarnNumbersFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto warning =
	    state.GetContext().registered_state->GetOrCreate<DecideNumberWarningState>("decide_number_warning_state");
	if (!warning->said.exchange(true)) {
		fprintf(stderr, "%s\n", DECIDE_NUMBER_RANGE_WARNING);
	}
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
