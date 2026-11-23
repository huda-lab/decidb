#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

DirectSourceFacts::DirectSourceFacts(const LogicalDecide &decide) {
	auto &source = *decide.children[0];
	if (source.type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		left = source.children[0]->GetColumnBindings();
		right = source.children[1]->GetColumnBindings();
	}
	ReadTableFacts(source);
}

void DirectSourceFacts::ReadTableFacts(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_FILTER || op.type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		for (auto &child : op.children) {
			ReadTableFacts(*child);
		}
		return;
	}
	if (op.type != LogicalOperatorType::LOGICAL_GET || !op.Cast<LogicalGet>().GetTable()) {
		return;
	}
	auto &get = op.Cast<LogicalGet>();
	auto &table = *get.GetTable();
	auto &column_ids = get.GetColumnIds();
	auto first_not_null = not_null.size();
	for (auto &constraint : table.GetConstraints()) {
		if (constraint->type != ConstraintType::NOT_NULL) {
			continue;
		}
		auto declared = constraint->Cast<NotNullConstraint>().index.index;
		for (idx_t i = 0; i < column_ids.size(); i++) {
			if (column_ids[i].GetPrimaryIndex() == declared) {
				not_null.emplace_back(get.table_index, i);
			}
		}
	}
	// A UNIQUE constraint, the PRIMARY KEY included, is a key when the scan reads all its columns and the table
	// declares them all NOT NULL: UNIQUE lets NULLs repeat. A NOT NULL binding is a column the scan reads.
	auto declared_and_read = [&](idx_t column, ColumnBinding &binding) {
		for (auto at = not_null.begin() + NumericCast<int64_t>(first_not_null); at != not_null.end(); ++at) {
			if (column_ids[at->column_index].GetPrimaryIndex() == column) {
				binding = *at;
				return true;
			}
		}
		return false;
	};
	for (auto &constraint : table.GetConstraints()) {
		if (constraint->type != ConstraintType::UNIQUE) {
			continue;
		}
		auto columns = constraint->Cast<UniqueConstraint>().GetLogicalIndexes(table.GetColumns());
		vector<ColumnBinding> key(columns.size());
		bool is_key = true;
		for (idx_t i = 0; i < columns.size(); i++) {
			is_key = is_key && declared_and_read(columns[i].index, key[i]);
		}
		if (is_key) {
			keys.push_back(std::move(key));
		}
	}
}

} // namespace duckdb
