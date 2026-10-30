#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"

#include "duckdb/common/enums/cte_materialize.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_late_materialization.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

//! The one constant of the choice: what a read of the materialized copy costs, as a share of re-scanning the chain once
//! (no decompression, filters or projections to redo). Re-scanning costs `reads` units; materializing costs one scan of
//! the chain, writing the copy (one copy-read) and `reads` copy-reads. At 0.4, R1's four reads cost 4 against 3, and
//! two reads cost 2 against 2.2, so a source is copied only when it is read three times or more. Any constant from 1/3
//! to 1/2 makes that choice. Measured on a copy of the columns the plan reads over partsupp, lineitem and a 20-column
//! table at 100K and 1M rows: with two reads the copy took 1.0 to 1.2 of the re-scans' time (and 3.5 to 4.3 times at 1M
//! rows when the whole source comes back), with three 0.57 to 0.98, with four 0.62 to 1.05.
static constexpr double COPY_READ_COST = 0.4;
//! The copy may take at most 1/8 of the memory the buffer manager has. A bigger one spills along with the sort and the
//! joins above it, and then the re-scans win. Measured at 800K rows with a copy of every column of the source:
//! partsupp's tied the re-scans at 1/11 of the memory, lost 5% at 1/7 and 14% at 1/4; the 20-column table whose parent
//! reads every column tied at 1/1.5, lost 8% at 1/1.2 and 44% at 1/0.7. 1/8 re-scans wherever the copy could lose, and
//! gives up the copy's win on a big table under a small memory limit.
static constexpr idx_t COPY_MEMORY_SHARE = 8;

bool DirectSourceCopyIsCheaper(idx_t reads, idx_t rows, idx_t row_width, idx_t memory) {
	if (!(1.0 + COPY_READ_COST * static_cast<double>(reads + 1) < static_cast<double>(reads))) {
		return false;
	}
	// Nothing is known of an empty or unestimated source: it is re-scanned.
	return rows > 0 && rows <= memory / COPY_MEMORY_SHARE / MaxValue<idx_t>(row_width, 1);
}

//! The width of one row of a copy of the source, from the physical size of its columns; a variable-width column counts
//! its 16 inline bytes. The copy leaves out the variable-width columns that `stored_columns` marks as late
//! materialized. The row id, which the source carries after its columns, is in the copy.
static idx_t CopyWidth(const vector<LogicalType> &types, const vector<idx_t> &stored_columns) {
	idx_t width = 0;
	for (idx_t i = 0; i < types.size(); i++) {
		auto physical = types[i].InternalType();
		if (TypeIsConstantSize(physical)) {
			width += GetTypeIdSize(physical);
		} else if (i >= stored_columns.size() || stored_columns[i] == DConstants::INVALID_INDEX) {
			width += sizeof(string_t);
		}
	}
	return width;
}

DirectSourceReader::DirectSourceReader(Optimizer &optimizer_p, unique_ptr<LogicalOperator> source_p, idx_t reads_p,
                                       const vector<idx_t> &stored_columns)
    : optimizer(optimizer_p), source(std::move(source_p)), reads(reads_p) {
	source->ResolveOperatorTypes();
	bindings = source->GetColumnBindings();
	types = source->types;
	D_ASSERT(bindings.size() == types.size());
	source_width = bindings.size();
	chain_scan = DirectRepeatableSourceChain(*source, chain);
	if (chain_scan && DirectSourceCopyIsCheaper(reads, source->EstimateCardinality(optimizer.context),
	                                            CopyWidth(types, stored_columns),
	                                            BufferManager::GetBufferManager(optimizer.context).GetMaxMemory())) {
		chain_scan = nullptr;
		chain.clear();
	}
	// A source read once is read as itself, as DuckDB inlines a CTE referenced once.
	materialized = !chain_scan && reads > 1;
	if (materialized) {
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

//! A side table's value and keys are columns of its definition, in this order, as a CTE's columns are named by
//! position.
static vector<string> PositionalNames(idx_t count) {
	vector<string> names;
	for (idx_t i = 0; i < count; i++) {
		names.push_back("c" + std::to_string(i));
	}
	return names;
}

void DirectSourceReader::AddSideTable(unique_ptr<LogicalOperator> definition, vector<unique_ptr<Expression>> keys,
                                      ColumnBinding placeholder) {
	// A table added after a gate would be gated, and the verdict would read it.
	D_ASSERT(verdicts.empty());
	definition->ResolveOperatorTypes();
	D_ASSERT(definition->types.size() == keys.size() + 1 && placeholder.column_index == keys.size());
	// The value is read where the clauses' expressions have the placeholder.
	bindings.push_back(placeholder);
	types.push_back(definition->types.back());
	SideTable table;
	table.types = definition->types;
	table.definition = std::move(definition);
	table.keys = std::move(keys);
	table.cte_index = optimizer.binder.GenerateTableIndex();
	table.ref_index = placeholder.table_index;
	side_tables.push_back(std::move(table));
}

unique_ptr<LogicalOperator> DirectSourceReader::JoinSideTables(unique_ptr<LogicalOperator> top, bool own_bindings,
                                                               vector<ColumnBinding> &side_values) {
	if (side_tables.empty()) {
		return top;
	}
	ExpressionBuilder calc(optimizer.context);
	top->ResolveOperatorTypes();
	// The key is written over the source's columns, which `top` has first, under its own bindings.
	auto top_bindings = top->GetColumnBindings();
	D_ASSERT(top_bindings.size() >= source_width);
	vector<ColumnBinding> source_columns(bindings.begin(), bindings.begin() + NumericCast<int64_t>(source_width));
	vector<unique_ptr<Expression>> on_top;
	for (idx_t i = 0; i < source_width; i++) {
		on_top.push_back(calc.Out(calc.Col(types[i], top_bindings[i])));
	}
	for (auto &table : side_tables) {
		auto ref_index = own_bindings ? table.ref_index : optimizer.binder.GenerateTableIndex();
		auto reference =
		    make_uniq<LogicalCTERef>(ref_index, table.cte_index, table.types, PositionalNames(table.types.size()),
		                             CTEMaterialize::CTE_MATERIALIZE_ALWAYS);
		side_values.emplace_back(ref_index, table.keys.size());
		if (table.keys.empty()) {
			// One group: its one row.
			top = LogicalCrossProduct::Create(std::move(top), std::move(reference));
			continue;
		}
		// The form DuckDB gives each row of a scalar subquery its one matching row: every row's group is in the table.
		auto join = make_uniq<LogicalComparisonJoin>(JoinType::SINGLE);
		for (idx_t j = 0; j < table.keys.size(); j++) {
			JoinCondition on_key;
			on_key.comparison = ExpressionType::COMPARE_NOT_DISTINCT_FROM;
			on_key.left = table.keys[j]->Copy();
			DirectReplaceColumns(on_key.left, source_columns, on_top);
			on_key.right = calc.Out(calc.Col(table.types[j], ColumnBinding(ref_index, j)));
			join->conditions.push_back(std::move(on_key));
		}
		join->children.push_back(std::move(top));
		join->children.push_back(std::move(reference));
		top = std::move(join);
	}
	return top;
}

DirectSourceReader::Read DirectSourceReader::ForwardAllColumns(unique_ptr<LogicalOperator> top,
                                                               const vector<ColumnBinding> &side_values) {
	ExpressionBuilder calc(optimizer.context);
	top->ResolveOperatorTypes();
	auto top_bindings = top->GetColumnBindings();
	D_ASSERT(top_bindings.size() >= source_width && side_values.size() == side_tables.size());
	Read read;
	read.table_index = optimizer.binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> expressions;
	for (idx_t i = 0; i < source_width; i++) {
		expressions.push_back(calc.Out(calc.Col(top->types[i], top_bindings[i])));
	}
	for (idx_t i = 0; i < side_values.size(); i++) {
		expressions.push_back(calc.Out(calc.Col(types[source_width + i], side_values[i])));
	}
	auto projection = make_uniq<LogicalProjection>(read.table_index, std::move(expressions));
	projection->children.push_back(std::move(top));
	projection->ResolveOperatorTypes();
	read.plan = std::move(projection);
	return read;
}

DirectSourceReader::Read DirectSourceReader::ReadSource() {
	// A copy reads the chain, which points into the source.
	D_ASSERT(source);
	unique_ptr<LogicalOperator> rows;
	if (chain_scan) {
		rows = CopyOfChain();
	} else {
		// A source read once has only the read TakeRecomputedSource makes.
		D_ASSERT(materialized);
		vector<LogicalType> source_types(types.begin(), types.begin() + NumericCast<int64_t>(source_width));
		rows = make_uniq<LogicalCTERef>(optimizer.binder.GenerateTableIndex(), cte_index, std::move(source_types),
		                                PositionalNames(source_width), CTEMaterialize::CTE_MATERIALIZE_ALWAYS);
	}
	vector<ColumnBinding> side_values;
	rows = Gated(JoinSideTables(std::move(rows), false, side_values));
	return ForwardAllColumns(std::move(rows), side_values);
}

unique_ptr<LogicalOperator> DirectSourceReader::TakeRecomputedSource() {
	if (materialized) {
		return nullptr;
	}
	vector<ColumnBinding> side_values;
	return Gated(JoinSideTables(std::move(source), true, side_values));
}

void DirectSourceReader::Gate(unique_ptr<LogicalOperator> verdict_p) {
	D_ASSERT(verdict_p);
	verdict_p->ResolveOperatorTypes();
	D_ASSERT(verdict_p->types.size() == 1 && verdict_p->types[0] == LogicalType::BOOLEAN);
	Verdict verdict;
	verdict.definition = std::move(verdict_p);
	verdict.cte_index = optimizer.binder.GenerateTableIndex();
	verdicts.push_back(std::move(verdict));
}

unique_ptr<LogicalOperator> DirectSourceReader::Gated(unique_ptr<LogicalOperator> read) {
	if (verdicts.empty()) {
		return read;
	}
	// Only the newest verdict is checked: its definition read rows that passed the one before it.
	ExpressionBuilder calc(optimizer.context);
	auto reference_index = optimizer.binder.GenerateTableIndex();
	auto reference =
	    make_uniq<LogicalCTERef>(reference_index, verdicts.back().cte_index, vector<LogicalType> {LogicalType::BOOLEAN},
	                             vector<string> {"v"}, CTEMaterialize::CTE_MATERIALIZE_ALWAYS);
	// The filter reads the verdict, so no pass prunes it. It passes on the read's bindings and the verdict's.
	auto gate = make_uniq<LogicalFilter>(calc.Out(calc.Col(LogicalType::BOOLEAN, ColumnBinding(reference_index, 0))));
	gate->children.push_back(LogicalCrossProduct::Create(std::move(read), std::move(reference)));
	return std::move(gate);
}

unique_ptr<LogicalOperator> DirectSourceReader::Finish(unique_ptr<LogicalOperator> plan) {
	// The first verdict is the outermost, because a later one's definition reads it.
	for (idx_t i = verdicts.size(); i > 0; i--) {
		auto &verdict = verdicts[i - 1];
		plan = make_uniq<LogicalMaterializedCTE>("direct_verdict", verdict.cte_index, 1, std::move(verdict.definition),
		                                         std::move(plan));
	}
	// The first side table is the outermost, because a later one's definition may read it.
	for (idx_t i = side_tables.size(); i > 0; i--) {
		auto &table = side_tables[i - 1];
		plan = make_uniq<LogicalMaterializedCTE>("direct_bound", table.cte_index, table.types.size(),
		                                         std::move(table.definition), std::move(plan));
	}
	if (!materialized) {
		return plan;
	}
	// A reference reads the materialized columns by position, but a join's output order can change in the optimizer
	// passes after this one (a pass that swaps its sides), so the definition names its columns in a projection, as the
	// binder does for the query of every CTE.
	ExpressionBuilder calc(optimizer.context);
	vector<unique_ptr<Expression>> columns;
	for (idx_t i = 0; i < source_width; i++) {
		columns.push_back(calc.Out(calc.Col(types[i], bindings[i])));
	}
	auto definition = make_uniq<LogicalProjection>(optimizer.binder.GenerateTableIndex(), std::move(columns));
	definition->children.push_back(std::move(source));
	return make_uniq<LogicalMaterializedCTE>("direct_source", cte_index, source_width, std::move(definition),
	                                         std::move(plan));
}

} // namespace duckdb
