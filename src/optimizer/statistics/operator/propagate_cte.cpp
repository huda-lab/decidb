#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"

namespace duckdb {

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalMaterializedCTE &cte,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	// the definition first: the references below it read what it proves of each column
	PropagateStatistics(cte.children[0]);
	vector<CTEColumn> columns;
	for (auto &binding : cte.children[0]->GetColumnBindings()) {
		CTEColumn column;
		auto stats = statistics_map.find(binding);
		if (stats != statistics_map.end()) {
			column.stats = stats->second->ToUnique();
		}
		Value value;
		if (TryGetConstantColumn(binding, value)) {
			column.constant = make_uniq<Value>(std::move(value));
		}
		columns.push_back(std::move(column));
	}
	cte_columns[cte.table_index] = std::move(columns);
	PropagateStatistics(cte.children[1]);
	return nullptr;
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalCTERef &ref,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	// only a materialized CTE has recorded its columns: a reference to a recursive CTE reads the rows the previous
	// iteration produced, which the definition's first iteration does not describe
	auto found = cte_columns.find(ref.cte_index);
	if (found == cte_columns.end()) {
		return nullptr;
	}
	auto &columns = found->second;
	D_ASSERT(columns.size() == ref.chunk_types.size());
	for (idx_t i = 0; i < columns.size(); i++) {
		ColumnBinding binding(ref.table_index, i);
		if (columns[i].stats) {
			statistics_map[binding] = columns[i].stats->ToUnique();
		}
		if (columns[i].constant) {
			constant_columns[binding] = *columns[i].constant;
		}
	}
	return nullptr;
}

} // namespace duckdb
