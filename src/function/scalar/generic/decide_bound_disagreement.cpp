#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"

namespace duckdb {

namespace {

//! The arguments are the bound as written, its two values, then each key column of the group as its name (a constant)
//! and its value on the row. DecideBoundDisagreementMessage states the text, as the solver path does.
void DecideBoundDisagreementFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetData<string_t>(result);
	for (idx_t row = 0; row < args.size(); row++) {
		vector<string> names;
		vector<Value> values;
		for (idx_t column = 3; column + 1 < args.ColumnCount(); column += 2) {
			names.push_back(args.data[column].GetValue(row).ToString());
			values.push_back(args.data[column + 1].GetValue(row));
		}
		out[row] = StringVector::AddString(
		    result, DecideBoundDisagreementMessage(args.data[0].GetValue(row).ToString(),
		                                           args.data[1].GetValue(row).GetValue<double>(),
		                                           args.data[2].GetValue(row).GetValue<double>(), names, values));
	}
}

} // namespace

ScalarFunction InternalDecideBoundDisagreementFun::GetFunction() {
	ScalarFunction fun("__internal_decide_bound_disagreement",
	                   {LogicalType::VARCHAR, LogicalType::DOUBLE, LogicalType::DOUBLE}, LogicalType::VARCHAR,
	                   DecideBoundDisagreementFunction);
	fun.varargs = LogicalType::ANY;
	// A key value may be NULL (`g IS NULL`), which the message names.
	fun.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return fun;
}

} // namespace duckdb
