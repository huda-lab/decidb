#include "duckdb/optimizer/decide/direct/direct_late_materialization.hpp"

#include <algorithm>

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/optimizer/decide/direct/direct_builder.hpp"
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
	if (get.function.function != TableScanFunction::GetFunction().function || !get.GetTable() ||
	    !get.GetTable()->IsDuckTable()) {
		return false;
	}
	return get.children.empty() && !get.extra_info.sample_options;
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
		if (binding.table_index != projection.table_index || binding.column_index >= projection.expressions.size()) {
			return DConstants::INVALID_INDEX;
		}
		auto &expression = *projection.expressions[binding.column_index];
		if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return DConstants::INVALID_INDEX;
		}
		auto &ref = expression.Cast<BoundColumnRefExpression>();
		if (ref.depth != 0) {
			return DConstants::INVALID_INDEX;
		}
		binding = ref.binding;
	}
	if (binding.table_index != get.table_index || binding.column_index >= get.GetColumnIds().size()) {
		return DConstants::INVALID_INDEX;
	}
	auto &column = get.GetColumnIds()[binding.column_index];
	if (column.IsRowIdColumn() || column.HasChildren()) {
		return DConstants::INVALID_INDEX;
	}
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
//! not follow: projections, filters, windows, orders and the scan are the ones a rule's plan is made of.
bool CollectPlanOperators(LogicalOperator &op, vector<reference<LogicalOperator>> &operators) {
	for (auto &child : op.children) {
		switch (child->type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
		case LogicalOperatorType::LOGICAL_WINDOW:
		case LogicalOperatorType::LOGICAL_ORDER_BY:
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

//! True when the stored column `column`, read at `source_binding` by the scan, reaches the boundary output only by
//! being forwarded: every use of it or of an alias of it in the plan is a projection that passes it on unchanged, and
//! the boundary's output `output` is the only dependency that reads it.
bool IsOnlyForwarded(LogicalDirectSolveResult &boundary, idx_t output, ColumnBinding source_binding,
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
				if (ref.depth == 0 && aliases.find(ref.binding) != aliases.end() &&
				    aliases.insert(ColumnBinding(projection.table_index, i)).second) {
					changed = true;
				}
			}
		}
	}
	// The boundary must read the column through an alias, and read it for this output alone.
	auto &dependency = *boundary.expressions[output];
	if (dependency.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
	    aliases.find(dependency.Cast<BoundColumnRefExpression>().binding) == aliases.end()) {
		return false;
	}
	for (idx_t i = 0; i < boundary.expressions.size(); i++) {
		if (i != output && RefersToAny(*boundary.expressions[i], aliases)) {
			return false;
		}
	}
	bool used = false;
	for (auto &entry : operators) {
		auto &op = entry.get();
		bool projection = op.type == LogicalOperatorType::LOGICAL_PROJECTION;
		idx_t index = 0;
		LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expression) {
			auto position = index++;
			if (used) {
				return;
			}
			if (projection && (*expression)->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				auto &ref = (*expression)->Cast<BoundColumnRefExpression>();
				if (ref.depth == 0 && aliases.find(ref.binding) != aliases.end() &&
				    aliases.find(ColumnBinding(op.Cast<LogicalProjection>().table_index, position)) != aliases.end()) {
					return; // forwarded unchanged
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

LogicalGet *FindScan(LogicalOperator &op, idx_t table_index) {
	if (op.type == LogicalOperatorType::LOGICAL_GET && op.Cast<LogicalGet>().table_index == table_index) {
		return &op.Cast<LogicalGet>();
	}
	for (auto &child : op.children) {
		auto found = FindScan(*child, table_index);
		if (found) {
			return found;
		}
	}
	return nullptr;
}

//! Removes unused columns again below a boundary, as if a parent read every output it still computes. The boundary
//! stays where it is.
void PruneBelowBoundary(unique_ptr<LogicalOperator> &boundary_slot, Optimizer &optimizer) {
	auto &boundary = boundary_slot->Cast<LogicalDirectSolveResult>();
	auto bindings = boundary.GetColumnBindings();
	vector<unique_ptr<Expression>> reads;
	for (idx_t i = 0; i < bindings.size(); i++) {
		reads.push_back(DirectColumn(boundary.output_types[i], bindings[i]));
	}
	auto reader = make_uniq<LogicalProjection>(optimizer.binder.GenerateTableIndex(), std::move(reads));
	reader->children.push_back(std::move(boundary_slot));
	RemoveUnusedColumns unused(optimizer.binder, optimizer.context, true);
	unused.VisitOperator(*reader);
	boundary_slot = std::move(reader->children[0]);
}

void LetGoOfRowId(LogicalDirectSolveResult &boundary) {
	boundary.expressions.pop_back();
	boundary.has_rowid_dependency = false;
	boundary.scan_table_index = DConstants::INVALID_INDEX;
	boundary.stored_columns.clear();
}

void LateMaterializeOneAdapter(unique_ptr<LogicalOperator> &slot, Optimizer &optimizer) {
	auto &boundary = slot->Cast<LogicalDirectSolveResult>();
	if (!boundary.has_rowid_dependency) {
		return;
	}
	if (boundary.expressions.size() != boundary.ExpectedDependencyCount() || boundary.children.size() != 1) {
		throw InternalException("Direct solve result boundary has invalid output dependencies");
	}
	auto &rowid_dependency = *boundary.expressions.back();
	if (rowid_dependency.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		// No parent read a column that could move; the row id was already dropped.
		LetGoOfRowId(boundary);
		return;
	}
	auto rowid_binding = rowid_dependency.Cast<BoundColumnRefExpression>().binding;
	auto rowid_type = rowid_dependency.return_type;
	vector<reference<LogicalOperator>> operators;
	auto scan = FindScan(*boundary.children[0], boundary.scan_table_index);
	bool supported = scan && IsRepeatableTableScan(*scan) && CollectPlanOperators(boundary, operators) &&
	                 rowid_type == scan->GetRowIdType();
	// Outputs a parent reads, whose stored column nothing else in the plan uses. Only a variable-width column moves
	// (VARCHAR, BLOB, BIT, lists, arrays, structs, maps, unions), because holding one is what costs memory. A
	// fixed-width column is cheaper held than read back, so it stays in the single read.
	vector<idx_t> moved_outputs;
	vector<idx_t> moved_columns;
	if (supported) {
		auto scan_bindings = scan->GetColumnBindings();
		for (idx_t i = 0; i < boundary.output_types.size(); i++) {
			if (i >= boundary.stored_columns.size() || boundary.stored_columns[i] == DConstants::INVALID_INDEX ||
			    boundary.expressions[i]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				continue;
			}
			if (TypeIsConstantSize(boundary.output_types[i].InternalType())) {
				continue;
			}
			auto stored = boundary.stored_columns[i];
			if (stored >= scan->returned_types.size() || scan->returned_types[stored] != boundary.output_types[i]) {
				continue;
			}
			bool found = false;
			ColumnBinding source_binding;
			for (auto &binding : scan_bindings) {
				auto &column = scan->GetColumnIds()[binding.column_index];
				if (!column.IsRowIdColumn() && !column.HasChildren() && column.GetPrimaryIndex() == stored) {
					source_binding = binding;
					found = true;
					break;
				}
			}
			if (found && IsOnlyForwarded(boundary, i, source_binding, operators)) {
				moved_outputs.push_back(i);
				moved_columns.push_back(stored);
			}
		}
	}
	if (moved_outputs.empty()) {
		boundary.expressions.back() = make_uniq<BoundConstantExpression>(Value(rowid_type));
		LetGoOfRowId(boundary);
		PruneBelowBoundary(slot, optimizer);
		return;
	}

	// The late materialization: the moved columns and the row id, from a fresh scan of the same table.
	auto second_index = optimizer.binder.GenerateTableIndex();
	auto second = make_uniq<LogicalGet>(second_index, scan->function, scan->bind_data->Copy(), scan->returned_types,
	                                    scan->names, scan->GetRowIdType());
	vector<idx_t> second_columns;
	for (auto column : moved_columns) {
		if (std::find(second_columns.begin(), second_columns.end(), column) == second_columns.end()) {
			second_columns.push_back(column);
		}
	}
	vector<ColumnIndex> column_ids;
	for (auto column : second_columns) {
		column_ids.emplace_back(column);
	}
	column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
	auto second_rowid = ColumnBinding(second_index, column_ids.size() - 1);
	second->SetColumnIds(std::move(column_ids));

	for (idx_t k = 0; k < moved_outputs.size(); k++) {
		auto position = NumericCast<idx_t>(std::find(second_columns.begin(), second_columns.end(), moved_columns[k]) -
		                                   second_columns.begin());
		boundary.expressions[moved_outputs[k]] =
		    DirectColumn(boundary.output_types[moved_outputs[k]], ColumnBinding(second_index, position));
	}

	auto join = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
	JoinCondition condition;
	condition.comparison = ExpressionType::COMPARE_EQUAL;
	condition.left = DirectColumn(rowid_type, second_rowid);
	condition.right = DirectColumn(rowid_type, rowid_binding);
	join->conditions.push_back(std::move(condition));
	join->children.push_back(std::move(second));
	join->children.push_back(std::move(boundary.children[0]));
	boundary.children[0] = std::move(join);
	LetGoOfRowId(boundary);
	PruneBelowBoundary(slot, optimizer);
}

void VisitForBoundaries(unique_ptr<LogicalOperator> &op, Optimizer &optimizer) {
	if (op->type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR &&
	    op->Cast<LogicalExtensionOperator>().GetExtensionName() == DirectResultExtensionName()) {
		LateMaterializeOneAdapter(op, optimizer);
	}
	for (auto &child : op->children) {
		VisitForBoundaries(child, optimizer);
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
		if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
			if (op.Cast<LogicalFilter>().HasProjectionMap()) {
				return false;
			}
		} else if (op.type != LogicalOperatorType::LOGICAL_PROJECTION) {
			return false;
		}
		for (auto &expression : op.expressions) {
			if (expression->IsVolatile()) {
				return false;
			}
		}
		chain.push_back(op);
		current = *op.children[0];
	}
	auto &get = current.get().Cast<LogicalGet>();
	if (!IsRepeatableTableScan(get) || !get.projection_ids.empty()) {
		return false;
	}
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
	if (source.GetColumnBindings().size() != bindings.size() + 1 ||
	    source.GetColumnBindings().back() != rowid_binding) {
		throw InternalException("Direct solve could not append the source row id");
	}
	return true;
}

unique_ptr<LogicalOperator> ApplyDirectLateMaterialization(unique_ptr<LogicalOperator> plan, Optimizer &optimizer) {
	VisitForBoundaries(plan, optimizer);
	return plan;
}

} // namespace duckdb
