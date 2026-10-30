#include "duckdb/optimizer/decide/direct/direct_late_materialization.hpp"

#include <algorithm>

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/remove_unused_columns.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

namespace {

//! A scan the late materialization can repeat: a DuckDB base table read through the ordinary table scan, with nothing
//! that changes which rows it returns from one read to the next.
bool IsRepeatableTableScan(LogicalGet &get) {
	if (get.function.function != TableScanFunction::GetFunction().function) {
		return false;
	}
	// The table scan function reads a DuckDB table only. It has no child, and a TABLESAMPLE reaches the scan later
	// (SAMPLING_PUSHDOWN runs after this step), as a separate sample operator that a source chain never accepts.
	D_ASSERT(get.GetTable() && get.GetTable()->IsDuckTable());
	D_ASSERT(get.children.empty() && !get.extra_info.sample_options);
	return true;
}

//! The stored column a source column forwards unchanged, or invalid. `chain` runs from the source's top operator down
//! to (not including) the scan.
idx_t StoredColumnOf(const vector<reference<LogicalOperator>> &chain, LogicalGet &get, ColumnBinding binding) {
	for (auto &entry : chain) {
		auto &op = entry.get();
		if (op.type != LogicalOperatorType::LOGICAL_PROJECTION) {
			continue; // a filter passes its child's bindings through
		}
		auto &projection = op.Cast<LogicalProjection>();
		// The binding came from the operator above, so it names an output of this projection.
		D_ASSERT(binding.table_index == projection.table_index && binding.column_index < projection.expressions.size());
		auto &expression = *projection.expressions[binding.column_index];
		if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return DConstants::INVALID_INDEX;
		}
		auto &ref = expression.Cast<BoundColumnRefExpression>();
		D_ASSERT(ref.depth == 0); // a correlated reference is gone before the optimizer runs
		binding = ref.binding;
	}
	D_ASSERT(binding.table_index == get.table_index && binding.column_index < get.GetColumnIds().size());
	auto &column = get.GetColumnIds()[binding.column_index];
	// The caller returned early for a source that reads the row id, and no struct field is pushed into a scan yet.
	D_ASSERT(!column.IsRowIdColumn() && !column.HasChildren());
	return column.GetPrimaryIndex();
}

//! The scan whose row id `binding` carries, found by following the binding to the operator that makes it from another
//! binding: a projection of a column reference, and a reference to a materialized CTE (its column i is the
//! definition's output i). The rule's plan passes the row id along like any source column, through its own scan, a
//! recomputed read of it or the definition of a copy.
LogicalGet &FindScanOfRowId(LogicalOperator &plan, ColumnBinding binding) {
	unordered_map<idx_t, reference<LogicalOperator>> makers;
	std::function<void(LogicalOperator &)> collect = [&](LogicalOperator &op) {
		for (auto table_index : op.GetTableIndex()) {
			makers.emplace(table_index, op);
		}
		for (auto &child : op.children) {
			collect(*child);
		}
	};
	collect(plan);
	while (true) {
		auto maker = makers.find(binding.table_index);
		if (maker == makers.end()) {
			// Every binding on the row id's way is made by an operator of the plan.
			throw InternalException("Direct solve lost the row id of its source");
		}
		auto &op = maker->second.get();
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_GET:
			D_ASSERT(op.Cast<LogicalGet>().GetColumnIds()[binding.column_index].IsRowIdColumn());
			return op.Cast<LogicalGet>();
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			auto &expression = *op.Cast<LogicalProjection>().expressions[binding.column_index];
			D_ASSERT(expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF);
			binding = expression.Cast<BoundColumnRefExpression>().binding;
			break;
		}
		default: {
			D_ASSERT(op.type == LogicalOperatorType::LOGICAL_CTE_REF);
			auto definition = makers.find(op.Cast<LogicalCTERef>().cte_index);
			if (definition == makers.end()) {
				throw InternalException("Direct solve lost the definition of a copy of its source");
			}
			D_ASSERT(definition->second.get().type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE);
			binding = definition->second.get().children[0]->GetColumnBindings()[binding.column_index];
			break;
		}
		}
	}
}

//! Removes unused columns again below an adapter, as if a parent read every output it still computes. The adapter
//! stays where it is.
void PruneBelowAdapter(unique_ptr<LogicalOperator> &adapter_slot, Optimizer &optimizer) {
	ExpressionBuilder calc(optimizer.context);
	auto &adapter = adapter_slot->Cast<LogicalDirectResultAdapter>();
	auto bindings = adapter.GetColumnBindings();
	vector<unique_ptr<Expression>> reads;
	for (idx_t i = 0; i < bindings.size(); i++) {
		reads.push_back(calc.Out(calc.Col(adapter.output_types[i], bindings[i])));
	}
	auto reader = make_uniq<LogicalProjection>(optimizer.binder.GenerateTableIndex(), std::move(reads));
	reader->children.push_back(std::move(adapter_slot));
	RemoveUnusedColumns unused(optimizer.binder, optimizer.context, true);
	unused.VisitOperator(*reader);
	adapter_slot = std::move(reader->children[0]);
}

void LetGoOfRowId(LogicalDirectResultAdapter &adapter) {
	adapter.expressions.pop_back();
	adapter.has_rowid_dependency = false;
	adapter.stored_columns.clear();
}

void LateMaterializeOneAdapter(unique_ptr<LogicalOperator> &slot, Optimizer &optimizer) {
	ExpressionBuilder calc(optimizer.context);
	auto &adapter = slot->Cast<LogicalDirectResultAdapter>();
	if (!adapter.has_rowid_dependency) {
		return;
	}
	D_ASSERT(adapter.expressions.size() == adapter.ExpectedDependencyCount() && adapter.children.size() == 1);
	auto &rowid_dependency = *adapter.expressions.back();
	if (rowid_dependency.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		// No parent read a column that could move; the row id was already dropped.
		LetGoOfRowId(adapter);
		return;
	}
	auto rowid_binding = rowid_dependency.Cast<BoundColumnRefExpression>().binding;
	auto rowid_type = rowid_dependency.return_type;
	// The outputs the rule only forwards (the driver left out the stored columns its expressions read) that a parent
	// reads. Only a variable-width column moves (VARCHAR, BLOB, BIT, lists, arrays, structs, maps, unions), because
	// holding one is what costs memory. A fixed-width column is cheaper held than read back, so it stays in the single
	// read. Fetching a stored column by row id gives the value the plan forwards, so nothing below the adapter has to
	// be followed: the pruning below drops whatever no operator reads any more.
	vector<idx_t> moved_outputs;
	vector<idx_t> moved_columns;
	D_ASSERT(adapter.stored_columns.size() == adapter.output_types.size());
	for (idx_t i = 0; i < adapter.output_types.size(); i++) {
		if (adapter.stored_columns[i] == DConstants::INVALID_INDEX ||
		    adapter.expressions[i]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
		    TypeIsConstantSize(adapter.output_types[i].InternalType())) {
			continue;
		}
		moved_outputs.push_back(i);
		moved_columns.push_back(adapter.stored_columns[i]);
	}
	if (moved_outputs.empty()) {
		adapter.expressions.back() = calc.Out(calc.Constant(Value(rowid_type)));
		LetGoOfRowId(adapter);
		PruneBelowAdapter(slot, optimizer);
		return;
	}

	// The late materialization: the moved columns and the row id, from a fresh scan of the same table.
	auto second_index = optimizer.binder.GenerateTableIndex();
	auto &scan = FindScanOfRowId(adapter, rowid_binding);
	D_ASSERT(IsRepeatableTableScan(scan) && rowid_type == scan.GetRowIdType());
	auto second = make_uniq<LogicalGet>(second_index, scan.function, scan.bind_data->Copy(), scan.returned_types,
	                                    scan.names, scan.GetRowIdType());
	vector<ColumnIndex> column_ids;
	for (auto column : moved_columns) {
		column_ids.emplace_back(column);
	}
	column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
	auto second_rowid = ColumnBinding(second_index, column_ids.size() - 1);
	second->SetColumnIds(std::move(column_ids));

	for (idx_t k = 0; k < moved_outputs.size(); k++) {
		D_ASSERT(scan.returned_types[adapter.stored_columns[moved_outputs[k]]] ==
		         adapter.output_types[moved_outputs[k]]);
		adapter.expressions[moved_outputs[k]] =
		    calc.Out(calc.Col(adapter.output_types[moved_outputs[k]], ColumnBinding(second_index, k)));
	}

	auto join = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
	JoinCondition condition;
	condition.comparison = ExpressionType::COMPARE_EQUAL;
	condition.left = calc.Out(calc.Col(rowid_type, second_rowid));
	condition.right = calc.Out(calc.Col(rowid_type, rowid_binding));
	join->conditions.push_back(std::move(condition));
	join->children.push_back(std::move(second));
	join->children.push_back(std::move(adapter.children[0]));
	adapter.children[0] = std::move(join);
	LetGoOfRowId(adapter);
	PruneBelowAdapter(slot, optimizer);
}

void VisitForAdapters(unique_ptr<LogicalOperator> &op, Optimizer &optimizer) {
	if (op->type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR &&
	    op->Cast<LogicalExtensionOperator>().GetExtensionName() == DirectResultExtensionName()) {
		LateMaterializeOneAdapter(op, optimizer);
	}
	for (auto &child : op->children) {
		VisitForAdapters(child, optimizer);
	}
}

} // namespace

optional_ptr<LogicalGet> DirectRepeatableSourceChain(LogicalOperator &source,
                                                     vector<reference<LogicalOperator>> &chain) {
	reference<LogicalOperator> current = source;
	while (current.get().type != LogicalOperatorType::LOGICAL_GET) {
		auto &op = current.get();
		if (op.children.size() != 1) {
			return nullptr;
		}
		if (op.type != LogicalOperatorType::LOGICAL_FILTER && op.type != LogicalOperatorType::LOGICAL_PROJECTION) {
			return nullptr;
		}
		// Projection maps come from later passes (the IN-clause rewriter, column lifetime).
		D_ASSERT(!op.HasProjectionMap());
		for (auto &expression : op.expressions) {
			if (expression->IsVolatile()) {
				return nullptr;
			}
		}
		chain.push_back(op);
		current = *op.children[0];
	}
	auto &get = current.get().Cast<LogicalGet>();
	if (!IsRepeatableTableScan(get)) {
		return nullptr;
	}
	return get;
}

bool DirectExposeStoredRowId(ClientContext &context, LogicalOperator &source, const column_binding_set_t &read,
                             DirectLateMaterialization &late_materialization) {
	ExpressionBuilder calc(context);
	vector<reference<LogicalOperator>> chain;
	auto scan = DirectRepeatableSourceChain(source, chain);
	if (!scan) {
		return false;
	}
	auto &get = *scan;
	D_ASSERT(get.projection_ids.empty()); // set by the unused-column pass, which runs later
	auto &column_ids = get.GetMutableColumnIds();
	for (auto &column : column_ids) {
		if (column.IsRowIdColumn()) {
			return false; // the query already reads the row id; leave it alone
		}
	}
	auto bindings = source.GetColumnBindings();
	late_materialization.stored_columns.clear();
	for (auto &binding : bindings) {
		// A column the DECIDE node's expressions read is not only forwarded: the rule's plan holds it for its own use.
		late_materialization.stored_columns.push_back(
		    read.find(binding) == read.end() ? StoredColumnOf(chain, get, binding) : DConstants::INVALID_INDEX);
	}

	// Carry the table's row id up the chain as a trailing column of every operator on the way.
	column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
	auto rowid_binding = ColumnBinding(get.table_index, column_ids.size() - 1);
	for (idx_t i = chain.size(); i > 0; i--) {
		auto &op = chain[i - 1].get();
		if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
			auto &projection = op.Cast<LogicalProjection>();
			auto rowid = calc.Out(calc.Col(get.GetRowIdType(), rowid_binding));
			rowid->SetAlias("rowid");
			projection.expressions.push_back(std::move(rowid));
			rowid_binding = ColumnBinding(projection.table_index, projection.expressions.size() - 1);
		}
	}
	D_ASSERT(source.GetColumnBindings().size() == bindings.size() + 1 &&
	         source.GetColumnBindings().back() == rowid_binding);
	return true;
}

unique_ptr<LogicalOperator> ApplyDirectLateMaterialization(unique_ptr<LogicalOperator> plan, Optimizer &optimizer) {
	VisitForAdapters(plan, optimizer);
	return plan;
}

} // namespace duckdb
