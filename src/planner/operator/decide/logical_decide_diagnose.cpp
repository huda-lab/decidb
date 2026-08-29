#include "duckdb/planner/operator/decide/logical_decide_diagnose.hpp"

#include "duckdb/decidb/decide_diagnostic.hpp"

namespace duckdb {

LogicalDecideDiagnose::LogicalDecideDiagnose(idx_t table_index)
    : LogicalOperator(LogicalOperatorType::LOGICAL_DECIDE_DIAGNOSE), table_index(table_index) {
}

LogicalDecideDiagnose::LogicalDecideDiagnose()
    : LogicalOperator(LogicalOperatorType::LOGICAL_DECIDE_DIAGNOSE), table_index(DConstants::INVALID_INDEX) {
}

vector<ColumnBinding> LogicalDecideDiagnose::GetColumnBindings() {
	return GenerateColumnBindings(table_index, types.size());
}

void LogicalDecideDiagnose::ResolveTypes() {
	// The child's own columns never surface: DIAGNOSE reports on the run, it does not
	// return the query's rows. One schema, defined once beside the engine that fills it.
	vector<string> names;
	GetDecideDiagnoseSchema(names, types);
}

string LogicalDecideDiagnose::GetName() const {
	return "DECIDE_DIAGNOSE";
}

vector<idx_t> LogicalDecideDiagnose::GetTableIndex() const {
	return vector<idx_t> {table_index};
}

} // namespace duckdb
