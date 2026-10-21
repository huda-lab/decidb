#include "duckdb/optimizer/decide/direct/direct_late_materialization.hpp"

#include <algorithm>

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/function/table/table_scan.hpp"
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
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
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

bool RefersToAny(Expression &expression, const column_binding_set_t &bindings) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	    bindings.find(expression.Cast<BoundColumnRefExpression>().binding) != bindings.end()) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expression, [&](Expression &child) {
		if (!found) {
			found = RefersToAny(child, bindings);
		}
	});
	return found;
}

//! Every operator below `op`, `op` excluded. False when one of them is a kind whose handling of bindings this step does
//! not follow: projections, filters, windows and the scan are the ones a rule's plan is made of.
bool CollectPlanOperators(LogicalOperator &op, vector<reference<LogicalOperator>> &operators) {
	for (auto &child : op.children) {
		switch (child->type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
		case LogicalOperatorType::LOGICAL_WINDOW:
		case LogicalOperatorType::LOGICAL_GET:
			break;
		case LogicalOperatorType::LOGICAL_FILTER:
			if (child->Cast<LogicalFilter>().HasProjectionMap()) {
				return false;
			}
			break;
		default:
			return false;
		}
		operators.push_back(*child);
		if (!CollectPlanOperators(*child, operators)) {
			return false;
		}
	}
	return true;
}

//! True when the stored column `column`, read at `source_binding` by the scan, reaches the adapter output only by
//! being forwarded: every use of it or of an alias of it in the plan is a projection that passes it on unchanged, and
//! the adapter's output `output` is the only dependency that reads it.
bool IsOnlyForwarded(LogicalDirectResultAdapter &adapter, idx_t output, ColumnBinding source_binding,
                     const vector<reference<LogicalOperator>> &operators) {
	column_binding_set_t aliases;
	aliases.insert(source_binding);
	bool changed = true;
	while (changed) {
		changed = false;
		for (auto &entry : operators) {
			auto &op = entry.get();
			if (op.type != LogicalOperatorType::LOGICAL_PROJECTION) {
				continue;
			}
			auto &projection = op.Cast<LogicalProjection>();
			for (idx_t i = 0; i < projection.expressions.size(); i++) {
				auto &expression = *projection.expressions[i];
				if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
					continue;
				}
				auto &ref = expression.Cast<BoundColumnRefExpression>();
				D_ASSERT(ref.depth == 0);
				if (aliases.find(ref.binding) != aliases.end() &&
				    aliases.insert(ColumnBinding(projection.table_index, i)).second) {
					changed = true;
				}
			}
		}
	}
	// The adapter reads the column through an alias (the rule's plan passes a source column along unchanged), and it
	// reads it for this output alone.
	D_ASSERT(adapter.expressions[output]->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF);
	D_ASSERT(aliases.find(adapter.expressions[output]->Cast<BoundColumnRefExpression>().binding) != aliases.end());
	for (idx_t i = 0; i < adapter.expressions.size(); i++) {
		if (i != output && RefersToAny(*adapter.expressions[i], aliases)) {
			return false;
		}
	}
	bool used = false;
	for (auto &entry : operators) {
		auto &op = entry.get();
		bool projection = op.type == LogicalOperatorType::LOGICAL_PROJECTION;
		idx_t index = 0;
		LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expression) {
			index++;
			if (used) {
				return;
			}
			if (projection && (*expression)->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				auto &ref = (*expression)->Cast<BoundColumnRefExpression>();
				if (aliases.find(ref.binding) != aliases.end()) {
					// forwarded unchanged, so the closure above holds this output of the projection too
					D_ASSERT(aliases.find(ColumnBinding(op.Cast<LogicalProjection>().table_index, index - 1)) !=
					         aliases.end());
					return;
				}
			}
			used = RefersToAny(**expression, aliases);
		});
		if (used) {
			return false;
		}
	}
	return true;
}

//! The scan among the operators of a plan that CollectPlanOperators accepted. That plan is a chain, so it holds one.
LogicalGet &FindScan(const vector<reference<LogicalOperator>> &operators) {
	auto found = std::find_if(operators.begin(), operators.end(), [](const reference<LogicalOperator> &entry) {
		return entry.get().type == LogicalOperatorType::LOGICAL_GET;
	});
	D_ASSERT(found != operators.end());
	return found->get().Cast<LogicalGet>();
}

//! Removes unused columns again below an adapter, as if a parent read every output it still computes. The adapter
//! stays where it is.
void PruneBelowAdapter(unique_ptr<LogicalOperator> &adapter_slot, Optimizer &optimizer) {
	auto &adapter = adapter_slot->Cast<LogicalDirectResultAdapter>();
	auto bindings = adapter.GetColumnBindings();
	vector<unique_ptr<Expression>> reads;
	for (idx_t i = 0; i < bindings.size(); i++) {
		reads.push_back(DirectColumn(adapter.output_types[i], bindings[i]));
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
	adapter.scan_table_index = DConstants::INVALID_INDEX;
	adapter.stored_columns.clear();
}

void LateMaterializeOneAdapter(unique_ptr<LogicalOperator> &slot, Optimizer &optimizer) {
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
	vector<reference<LogicalOperator>> operators;
	bool supported = CollectPlanOperators(adapter, operators);
	// Outputs a parent reads, whose stored column nothing else in the plan uses. Only a variable-width column moves
	// (VARCHAR, BLOB, BIT, lists, arrays, structs, maps, unions), because holding one is what costs memory. A
	// fixed-width column is cheaper held than read back, so it stays in the single read.
	vector<idx_t> moved_outputs;
	vector<idx_t> moved_columns;
	optional_ptr<LogicalGet> first_scan;
	if (supported) {
		auto &scan = FindScan(operators);
		first_scan = &scan;
		D_ASSERT(scan.table_index == adapter.scan_table_index);
		D_ASSERT(IsRepeatableTableScan(scan) && rowid_type == scan.GetRowIdType());
		D_ASSERT(adapter.stored_columns.size() == adapter.output_types.size());
		auto scan_bindings = scan.GetColumnBindings();
		for (idx_t i = 0; i < adapter.output_types.size(); i++) {
			if (adapter.stored_columns[i] == DConstants::INVALID_INDEX ||
			    adapter.expressions[i]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				continue;
			}
			if (TypeIsConstantSize(adapter.output_types[i].InternalType())) {
				continue;
			}
			// A parent-read output that forwards a stored column is read by the scan under that column's own type.
			auto stored = adapter.stored_columns[i];
			D_ASSERT(stored < scan.returned_types.size() && scan.returned_types[stored] == adapter.output_types[i]);
			auto source = std::find_if(scan_bindings.begin(), scan_bindings.end(), [&](const ColumnBinding &binding) {
				return scan.GetColumnIds()[binding.column_index].GetPrimaryIndex() == stored;
			});
			D_ASSERT(source != scan_bindings.end() && !scan.GetColumnIds()[source->column_index].HasChildren());
			if (IsOnlyForwarded(adapter, i, *source, operators)) {
				moved_outputs.push_back(i);
				moved_columns.push_back(stored);
			}
		}
	}
	if (moved_outputs.empty()) {
		adapter.expressions.back() = make_uniq<BoundConstantExpression>(Value(rowid_type));
		LetGoOfRowId(adapter);
		PruneBelowAdapter(slot, optimizer);
		return;
	}

	// The late materialization: the moved columns and the row id, from a fresh scan of the same table.
	auto second_index = optimizer.binder.GenerateTableIndex();
	auto &scan = *first_scan;
	auto second = make_uniq<LogicalGet>(second_index, scan.function, scan.bind_data->Copy(), scan.returned_types,
	                                    scan.names, scan.GetRowIdType());
	// IsOnlyForwarded lets one output alone read a stored column, so no column moves twice.
	D_ASSERT(unordered_set<idx_t>(moved_columns.begin(), moved_columns.end()).size() == moved_columns.size());
	vector<ColumnIndex> column_ids;
	for (auto column : moved_columns) {
		column_ids.emplace_back(column);
	}
	column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
	auto second_rowid = ColumnBinding(second_index, column_ids.size() - 1);
	second->SetColumnIds(std::move(column_ids));

	for (idx_t k = 0; k < moved_outputs.size(); k++) {
		adapter.expressions[moved_outputs[k]] =
		    DirectColumn(adapter.output_types[moved_outputs[k]], ColumnBinding(second_index, k));
	}

	auto join = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
	JoinCondition condition;
	condition.comparison = ExpressionType::COMPARE_EQUAL;
	condition.left = DirectColumn(rowid_type, second_rowid);
	condition.right = DirectColumn(rowid_type, rowid_binding);
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

bool DirectExposeStoredRowId(LogicalOperator &source, DirectLateMaterialization &late_materialization) {
	vector<reference<LogicalOperator>> chain;
	reference<LogicalOperator> current = source;
	while (current.get().type != LogicalOperatorType::LOGICAL_GET) {
		auto &op = current.get();
		if (op.children.size() != 1) {
			return false;
		}
		if (op.type != LogicalOperatorType::LOGICAL_FILTER && op.type != LogicalOperatorType::LOGICAL_PROJECTION) {
			return false;
		}
		// Projection maps come from later passes (the IN-clause rewriter, column lifetime).
		D_ASSERT(!op.HasProjectionMap());
		for (auto &expression : op.expressions) {
			if (expression->IsVolatile()) {
				return false;
			}
		}
		chain.push_back(op);
		current = *op.children[0];
	}
	auto &get = current.get().Cast<LogicalGet>();
	if (!IsRepeatableTableScan(get)) {
		return false;
	}
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
		late_materialization.stored_columns.push_back(StoredColumnOf(chain, get, binding));
	}
	late_materialization.scan_table_index = get.table_index;

	// Carry the table's row id up the chain as a trailing column of every operator on the way.
	column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
	auto rowid_binding = ColumnBinding(get.table_index, column_ids.size() - 1);
	for (idx_t i = chain.size(); i > 0; i--) {
		auto &op = chain[i - 1].get();
		if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
			auto &projection = op.Cast<LogicalProjection>();
			projection.expressions.push_back(
			    make_uniq<BoundColumnRefExpression>("rowid", get.GetRowIdType(), rowid_binding));
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
