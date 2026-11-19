#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"

namespace duckdb {

namespace {

//! The arguments are the bound as written, its two values, where the bound stands in the query (NULL for a bound the
//! query text holds no place for), then each key column of the group as its name (a constant) and its value on the
//! row. A row where the two values differ raises the error of DecideBoundDisagreementMessage, at the bound, as the
//! solver path does; it is evaluated only on such rows (`CASE WHEN low <> high THEN raise`), so it returns nothing.
void DecideBoundDisagreementFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, true);
	if (args.size() == 0) {
		return;
	}
	// Any row of the chunk is one the check raised on; the first names the group.
	vector<string> names;
	vector<Value> values;
	for (idx_t column = 4; column + 1 < args.ColumnCount(); column += 2) {
		names.push_back(args.data[column].GetValue(0).ToString());
		values.push_back(args.data[column + 1].GetValue(0));
	}
	auto message =
	    DecideBoundDisagreementMessage(args.data[0].GetValue(0).ToString(), args.data[1].GetValue(0).GetValue<double>(),
	                                   args.data[2].GetValue(0).GetValue<double>(), names, values);
	auto location = args.data[3].GetValue(0);
	if (location.IsNull()) {
		throw InvalidInputException(message);
	}
	throw InvalidInputException(message, Exception::InitializeExtraInfo(optional_idx(location.GetValue<uint64_t>())));
}

} // namespace

ScalarFunction InternalDecideBoundDisagreementFun::GetFunction() {
	ScalarFunction fun("__internal_decide_bound_disagreement",
	                   {LogicalType::VARCHAR, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::UBIGINT},
	                   LogicalType::VARCHAR, DecideBoundDisagreementFunction);
	fun.varargs = LogicalType::ANY;
	// A key value may be NULL (`g IS NULL`), which the message names; so may the place.
	fun.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	BaseScalarFunction::SetReturnsError(fun);
	return fun;
}

} // namespace duckdb
