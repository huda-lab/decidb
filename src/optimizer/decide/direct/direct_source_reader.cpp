#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"

#include "duckdb/common/enums/cte_materialize.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_late_materialization.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

//! The one constant of the choice: what a read of the materialized copy costs, as a share of re-scanning the chain once
//! (no decompression, filters or projections to redo). Re-scanning costs `reads` units; materializing costs one scan of
//! the chain, writing the copy (one copy-read) and `reads` copy-reads. At 0.4, R1's four reads cost 4 against 3, and
//! two reads cost 2 against 2.2, so a source is copied only when it is read three times or more. Set from R1 on
//! partsupp and on a 20-column table at 100K and 1M rows, where one copy ran at 0.70 to 0.96 of four re-scans.
static constexpr double COPY_READ_COST = 0.4;
//! The copy may take at most 1/8 of the memory the buffer manager has. A bigger one spills along with the sort and the
//! joins above it, and then the re-scans win. Measured at 800K rows: partsupp's copy (the width of its source, which
//! counts a column the query never reads) tied the re-scans at 1/11 of the memory, lost 5% at 1/7 and 14% at 1/4; the
//! 20-column table whose parent reads every column tied at 1/1.5, lost 8% at 1/1.2 and 44% at 1/0.7. 1/8 re-scans
//! wherever the copy could lose, and gives up the copy's win on a big table under a small memory limit.
static constexpr idx_t COPY_MEMORY_SHARE = 8;

bool DirectSourceCopyIsCheaper(idx_t reads, idx_t rows, idx_t row_width, idx_t memory) {
	if (!(1.0 + COPY_READ_COST * static_cast<double>(reads + 1) < static_cast<double>(reads))) {
		return false;
	}
	// Nothing is known of an empty or unestimated source: it is re-scanned.
	return rows > 0 && rows <= memory / COPY_MEMORY_SHARE / MaxValue<idx_t>(row_width, 1);
}

//! The width of one row of the source, from the physical size of its columns; a variable-width column counts its 16
//! inline bytes.
static idx_t RowWidth(const vector<LogicalType> &types) {
	idx_t width = 0;
	for (auto &type : types) {
		auto physical = type.InternalType();
		width += TypeIsConstantSize(physical) ? GetTypeIdSize(physical) : sizeof(string_t);
	}
	return width;
}

DirectSourceReader::DirectSourceReader(Optimizer &optimizer_p, unique_ptr<LogicalOperator> source_p, idx_t reads_p)
    : optimizer(optimizer_p), source(std::move(source_p)), reads(reads_p) {
	source->ResolveOperatorTypes();
	bindings = source->GetColumnBindings();
	types = source->types;
	D_ASSERT(bindings.size() == types.size());
	chain_scan = DirectRepeatableSourceChain(*source, chain);
	if (chain_scan && DirectSourceCopyIsCheaper(reads, source->EstimateCardinality(optimizer.context), RowWidth(types),
	                                            BufferManager::GetBufferManager(optimizer.context).GetMaxMemory())) {
		chain_scan = nullptr;
		chain.clear();
	}
	if (!chain_scan) {
		cte_index = optimizer.binder.GenerateTableIndex();
	}
}

//! A fresh scan of the same table: the same columns and filters under a new table index, as the second scan of late
//! materialization is built.
unique_ptr<LogicalOperator> DirectSourceReader::CopyOfScan() const {
	auto &scan = *chain_scan;
	D_ASSERT(scan.projection_ids.empty() && !scan.dynamic_filters); // both come from later passes
	auto copy = make_uniq<LogicalGet>(optimizer.binder.GenerateTableIndex(), scan.function, scan.bind_data->Copy(),
	                                  scan.returned_types, scan.names, scan.GetRowIdType());
	vector<ColumnIndex> column_ids = scan.GetColumnIds();
	copy->SetColumnIds(std::move(column_ids));
	// Filter pushdown ran before the DECIDE pass, so the scan may already hold filters that select the source's rows.
	for (auto &filter : scan.table_filters.filters) {
		copy->table_filters.filters[filter.first] = filter.second->Copy();
	}
	copy->parameters = scan.parameters;
	copy->named_parameters = scan.named_parameters;
	return std::move(copy);
}

//! The chain of the source over a fresh scan. A projection or filter reads its child's columns by position, and the
//! child's copy has the same columns in the same order under one new table index, so a copied expression is re-pointed
//! at that index.
unique_ptr<LogicalOperator> DirectSourceReader::CopyOfChain() const {
	auto current = CopyOfScan();
	for (idx_t i = chain.size(); i > 0; i--) {
		auto &op = chain[i - 1].get();
		auto old_bindings = op.children[0]->GetColumnBindings();
		auto new_bindings = current->GetColumnBindings();
		D_ASSERT(!new_bindings.empty() && old_bindings.size() == new_bindings.size());
		vector<unique_ptr<Expression>> expressions;
		for (auto &expression : op.expressions) {
			auto copy = expression->Copy();
			// Every reference of a chain operator names its child's output, which the copy has under the new index.
			bool remapped = DirectRemapSourceReferences(*copy, old_bindings, new_bindings[0].table_index);
			D_ASSERT(remapped);
			(void)remapped;
			expressions.push_back(std::move(copy));
		}
		unique_ptr<LogicalOperator> next;
		if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
			next = make_uniq<LogicalProjection>(optimizer.binder.GenerateTableIndex(), std::move(expressions));
		} else {
			auto filter = make_uniq<LogicalFilter>();
			filter->expressions = std::move(expressions);
			next = std::move(filter);
		}
		next->children.push_back(std::move(current));
		current = std::move(next);
	}
	return current;
}

DirectSourceReader::Read DirectSourceReader::ForwardAllColumns(unique_ptr<LogicalOperator> top) {
	ExpressionBuilder calc(optimizer.context);
	top->ResolveOperatorTypes();
	auto top_bindings = top->GetColumnBindings();
	D_ASSERT(top_bindings.size() == types.size());
	Read read;
	read.table_index = optimizer.binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> expressions;
	for (idx_t i = 0; i < top_bindings.size(); i++) {
		expressions.push_back(calc.Out(calc.Col(top->types[i], top_bindings[i])));
	}
	auto projection = make_uniq<LogicalProjection>(read.table_index, std::move(expressions));
	projection->children.push_back(std::move(top));
	projection->ResolveOperatorTypes();
	read.plan = std::move(projection);
	return read;
}

DirectSourceReader::Read DirectSourceReader::ReadSource() {
	if (chain_scan) {
		return ForwardAllColumns(CopyOfChain());
	}
	vector<string> names;
	for (idx_t i = 0; i < types.size(); i++) {
		names.push_back("c" + std::to_string(i));
	}
	return ForwardAllColumns(make_uniq<LogicalCTERef>(optimizer.binder.GenerateTableIndex(), cte_index, types,
	                                                  std::move(names), CTEMaterialize::CTE_MATERIALIZE_ALWAYS));
}

unique_ptr<LogicalOperator> DirectSourceReader::Finish(unique_ptr<LogicalOperator> plan) {
	if (chain_scan) {
		return plan;
	}
	// A reference reads the materialized columns by position, but a join's output order can change in the optimizer
	// passes after this one (a pass that swaps its sides), so the definition names its columns in a projection, as the
	// binder does for the query of every CTE.
	ExpressionBuilder calc(optimizer.context);
	vector<unique_ptr<Expression>> columns;
	for (idx_t i = 0; i < types.size(); i++) {
		columns.push_back(calc.Out(calc.Col(types[i], bindings[i])));
	}
	auto definition = make_uniq<LogicalProjection>(optimizer.binder.GenerateTableIndex(), std::move(columns));
	definition->children.push_back(std::move(source));
	return make_uniq<LogicalMaterializedCTE>("direct_source", cte_index, types.size(), std::move(definition),
	                                         std::move(plan));
}

} // namespace duckdb
