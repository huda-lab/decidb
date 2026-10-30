#include "duckdb/optimizer/remove_unused_columns.hpp"

#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/parser/parsed_data/vacuum_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_simple.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/function/scalar/struct_utils.hpp"

namespace duckdb {

void BaseColumnPruner::ReplaceBinding(ColumnBinding current_binding, ColumnBinding new_binding) {
	auto colrefs = column_references.find(current_binding);
	if (colrefs != column_references.end()) {
		for (auto &colref_p : colrefs->second.bindings) {
			auto &colref = colref_p.get();
			D_ASSERT(colref.binding == current_binding);
			colref.binding = new_binding;
		}
	}
}

template <class T>
void RemoveUnusedColumns::ClearUnusedExpressions(vector<T> &list, idx_t table_idx, bool replace) {
	idx_t offset = 0;
	for (idx_t col_idx = 0; col_idx < list.size(); col_idx++) {
		auto current_binding = ColumnBinding(table_idx, col_idx + offset);
		auto entry = column_references.find(current_binding);
		if (entry == column_references.end()) {
			// this entry is not referred to, erase it from the set of expressions
			list.erase_at(col_idx);
			offset++;
			col_idx--;
		} else if (offset > 0 && replace) {
			// column is used but the ColumnBinding has changed because of removed columns
			ReplaceBinding(current_binding, ColumnBinding(table_idx, col_idx));
		}
	}
}

RemoveUnusedColumns::RemoveUnusedColumns(Binder &binder, ClientContext &context, bool is_root)
    : binder(binder), context(context), everything_referenced(is_root), outermost(true),
      cte_reads(make_shared_ptr<cte_reads_map_t>()) {
}

RemoveUnusedColumns::RemoveUnusedColumns(RemoveUnusedColumns &parent, bool is_root)
    : binder(parent.binder), context(parent.context), everything_referenced(is_root), outermost(false),
      cte_reads(parent.cte_reads) {
}

//! The references to the materialized CTE `cte_index` below `op`.
static idx_t CountCTEReferences(LogicalOperator &op, idx_t cte_index) {
	idx_t count = op.type == LogicalOperatorType::LOGICAL_CTE_REF && op.Cast<LogicalCTERef>().cte_index == cte_index;
	for (auto &child : op.children) {
		count += CountCTEReferences(*child, cte_index);
	}
	return count;
}

unique_ptr<LogicalOperator> RemoveUnusedColumns::ProjectColumns(unique_ptr<LogicalOperator> child,
                                                                vector<idx_t> &columns) {
	if (columns.empty()) {
		// no column is read (a COUNT(*)): keep the first
		columns.push_back(0);
	}
	child->ResolveOperatorTypes();
	auto bindings = child->GetColumnBindings();
	vector<unique_ptr<Expression>> expressions;
	expressions.reserve(columns.size());
	for (auto column : columns) {
		expressions.push_back(make_uniq<BoundColumnRefExpression>(child->types[column], bindings[column]));
	}
	auto projection = make_uniq<LogicalProjection>(binder.GenerateTableIndex(), std::move(expressions));
	if (child->has_estimated_cardinality) {
		projection->SetEstimatedCardinality(child->estimated_cardinality);
	}
	projection->children.push_back(std::move(child));
	return std::move(projection);
}

void RemoveUnusedColumns::NarrowMaterializedCTE(LogicalOperator &op, CTEReads &reads) {
	for (idx_t i = 0; i < reads.width; i++) {
		if (reads.read[i]) {
			reads.kept.push_back(i);
		}
	}
	// The definition's root is wrapped in a projection of the kept columns, whatever the root is. A reference that
	// reads nothing still has a column.
	if (reads.kept.size() == reads.width) {
		reads.kept.clear();
		return;
	}
	op.children[0] = ProjectColumns(std::move(op.children[0]), reads.kept);
	op.Cast<LogicalMaterializedCTE>().column_count = reads.kept.size();
}

void RemoveUnusedColumns::NarrowCTEReferences(LogicalOperator &plan) {
	ColumnBindingReplacer replacer;
	// A filter's projection map (the IN clause rewriter's, the dependent join's) is positional over its child's output,
	// so it is stale once a reference below the filter has fewer columns. The filter keeps the columns it passed on.
	vector<pair<reference<LogicalFilter>, vector<ColumnBinding>>> mapped_filters;
	column_binding_set_t dropped;
	std::function<void(LogicalOperator &)> narrow = [&](LogicalOperator &op) {
		if (op.type == LogicalOperatorType::LOGICAL_FILTER && op.Cast<LogicalFilter>().HasProjectionMap()) {
			mapped_filters.emplace_back(op.Cast<LogicalFilter>(), op.GetColumnBindings());
		}
		if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
			auto &ref = op.Cast<LogicalCTERef>();
			auto entry = cte_reads->find(ref.cte_index);
			if (entry != cte_reads->end() && !entry->second.kept.empty()) {
				auto &kept = entry->second.kept;
				D_ASSERT(ref.chunk_types.size() == entry->second.width);
				vector<LogicalType> types;
				vector<string> names;
				for (idx_t column = 0; column < ref.chunk_types.size(); column++) {
					if (std::find(kept.begin(), kept.end(), column) == kept.end()) {
						dropped.insert(ColumnBinding(ref.table_index, column));
					}
				}
				for (idx_t position = 0; position < kept.size(); position++) {
					types.push_back(ref.chunk_types[kept[position]]);
					names.push_back(ref.bound_columns[kept[position]]);
					// A kept column never moves to a later position, so no replacement meets another's result.
					replacer.replacement_bindings.emplace_back(ColumnBinding(ref.table_index, kept[position]),
					                                           ColumnBinding(ref.table_index, position));
				}
				ref.chunk_types = std::move(types);
				ref.bound_columns = std::move(names);
			}
		}
		for (auto &child : op.children) {
			narrow(*child);
		}
	};
	narrow(plan);
	if (replacer.replacement_bindings.empty()) {
		return;
	}
	replacer.VisitOperator(plan);
	for (auto &entry : mapped_filters) {
		auto &filter = entry.first.get();
		auto child_bindings = filter.children[0]->GetColumnBindings();
		filter.projection_map.clear();
		for (auto binding : entry.second) {
			if (dropped.find(binding) != dropped.end()) {
				continue;
			}
			for (auto &replacement : replacer.replacement_bindings) {
				if (binding == replacement.old_binding) {
					binding = replacement.new_binding;
					break;
				}
			}
			auto position = std::find(child_bindings.begin(), child_bindings.end(), binding);
			D_ASSERT(position != child_bindings.end());
			filter.projection_map.push_back(NumericCast<idx_t>(position - child_bindings.begin()));
		}
	}
}

void RemoveUnusedColumns::VisitOperator(LogicalOperator &op) {
	if (outermost) {
		// Pruned in one pass; the CTE references are narrowed after it, once every read of them is known, because the
		// readers of a reference's columns are spread over the whole plan.
		outermost = false;
		VisitOperator(op);
		NarrowCTEReferences(op);
		return;
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		// aggregate
		if (!everything_referenced) {
			// FIXME: groups that are not referenced need to stay -> but they don't need to be scanned and output!
			auto &aggr = op.Cast<LogicalAggregate>();
			ClearUnusedExpressions(aggr.expressions, aggr.aggregate_index);
			if (aggr.expressions.empty() && aggr.groups.empty()) {
				// removed all expressions from the aggregate: push a COUNT(*)
				auto count_star_fun = CountStarFun::GetFunction();
				FunctionBinder function_binder(context);
				aggr.expressions.push_back(
				    function_binder.BindAggregateFunction(count_star_fun, {}, nullptr, AggregateType::NON_DISTINCT));
			}
		}

		// then recurse into the children of the aggregate
		RemoveUnusedColumns remove(*this);
		remove.VisitOperatorExpressions(op);
		remove.VisitOperator(*op.children[0]);
		return;
	}
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		if (!everything_referenced) {
			auto &comp_join = op.Cast<LogicalComparisonJoin>();

			if (comp_join.join_type != JoinType::INNER) {
				break;
			}
			// for inner joins with equality predicates in the form of (X=Y)
			// we can replace any references to the RHS (Y) to references to the LHS (X)
			// this reduces the amount of columns we need to extract from the join hash table
			for (auto &cond : comp_join.conditions) {
				if (cond.comparison == ExpressionType::COMPARE_EQUAL) {
					if (cond.left->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
					    cond.right->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
						// comparison join between two bound column refs
						// we can replace any reference to the RHS (build-side) with a reference to the LHS (probe-side)
						auto &lhs_col = cond.left->Cast<BoundColumnRefExpression>();
						auto &rhs_col = cond.right->Cast<BoundColumnRefExpression>();
						// if there are any columns that refer to the RHS,
						auto colrefs = column_references.find(rhs_col.binding);
						if (colrefs != column_references.end()) {
							for (auto &entry : colrefs->second.bindings) {
								auto &colref = entry.get();
								colref.binding = lhs_col.binding;
								AddBinding(colref);
							}
							column_references.erase(rhs_col.binding);
						}
					}
				}
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
		break;
	case LogicalOperatorType::LOGICAL_UNION: {
		auto &setop = op.Cast<LogicalSetOperation>();
		if (setop.setop_all && !everything_referenced) {
			// for UNION we can remove unreferenced columns if union all is used
			// it's possible not all columns are referenced, but unreferenced columns in the union can
			// still have an affect on the result of the union
			vector<idx_t> entries;
			for (idx_t i = 0; i < setop.column_count; i++) {
				entries.push_back(i);
			}
			ClearUnusedExpressions(entries, setop.table_index);
			if (entries.size() < setop.column_count) {
				for (idx_t child_idx = 0; child_idx < op.children.size(); child_idx++) {
					RemoveUnusedColumns remove(*this, true);

					// we push a projection under this child that references the required columns of the union
					op.children[child_idx] = ProjectColumns(std::move(op.children[child_idx]), entries);

					remove.VisitOperator(*op.children[child_idx]);
				}
				// columns were cleared
				setop.column_count = entries.size();
				return;
			}
		}
		for (auto &child : op.children) {
			RemoveUnusedColumns remove(*this, true);
			remove.VisitOperator(*child);
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT: {
		// for INTERSECT/EXCEPT operations we can't remove anything, just recursively visit the children
		for (auto &child : op.children) {
			RemoveUnusedColumns remove(*this, true);
			remove.VisitOperator(*child);
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		if (!everything_referenced) {
			auto &proj = op.Cast<LogicalProjection>();
			ClearUnusedExpressions(proj.expressions, proj.table_index);

			if (proj.expressions.empty()) {
				// nothing references the projected expressions
				// this happens in the case of e.g. EXISTS(SELECT * FROM ...)
				// in this case we only need to project a single constant
				proj.expressions.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(42)));
			}
		}
		// then recurse into the children of this projection
		RemoveUnusedColumns remove(*this);
		remove.VisitOperatorExpressions(op);
		remove.VisitOperator(*op.children[0]);
		return;
	}
	case LogicalOperatorType::LOGICAL_INSERT:
	case LogicalOperatorType::LOGICAL_UPDATE:
	case LogicalOperatorType::LOGICAL_DELETE: {
		//! When RETURNING is used, a PROJECTION is the top level operator for INSERTS, UPDATES, and DELETES
		//! We still need to project all values from these operators so the projection
		//! on top of them can select from only the table values being inserted.
		//! TODO: Push down the projections from the returning statement
		//! TODO: Be careful because you might be adding expressions when a user returns *
		RemoveUnusedColumns remove(*this, true);
		remove.VisitOperatorExpressions(op);
		remove.VisitOperator(*op.children[0]);
		return;
	}
	case LogicalOperatorType::LOGICAL_GET:
		LogicalOperatorVisitor::VisitOperatorExpressions(op);
		if (!everything_referenced) {
			auto &get = op.Cast<LogicalGet>();
			if (!get.function.projection_pushdown) {
				return;
			}

			auto final_column_ids = get.GetColumnIds();

			// Create "selection vector" of all column ids
			vector<idx_t> proj_sel;
			for (idx_t col_idx = 0; col_idx < final_column_ids.size(); col_idx++) {
				proj_sel.push_back(col_idx);
			}
			// Create a copy that we can use to match ids later
			auto col_sel = proj_sel;
			// Clear unused ids, exclude filter columns that are projected out immediately
			ClearUnusedExpressions(proj_sel, get.table_index, false);

			vector<unique_ptr<Expression>> filter_expressions;
			// for every table filter, push a column binding into the column references map to prevent the column from
			// being projected out
			for (auto &filter : get.table_filters.filters) {
				optional_idx index;
				for (idx_t i = 0; i < final_column_ids.size(); i++) {
					if (final_column_ids[i].GetPrimaryIndex() == filter.first) {
						index = i;
						break;
					}
				}
				if (!index.IsValid()) {
					throw InternalException("Could not find column index for table filter");
				}

				auto column_type =
				    filter.first == COLUMN_IDENTIFIER_ROW_ID ? LogicalType::ROW_TYPE : get.returned_types[filter.first];

				ColumnBinding filter_binding(get.table_index, index.GetIndex());
				auto column_ref = make_uniq<BoundColumnRefExpression>(std::move(column_type), filter_binding);
				auto filter_expr = filter.second->ToExpression(*column_ref);
				VisitExpression(&filter_expr);
				filter_expressions.push_back(std::move(filter_expr));
			}

			// Clear unused ids, include filter columns that are projected out immediately
			ClearUnusedExpressions(col_sel, get.table_index);

			// Now set the column ids in the LogicalGet using the "selection vector"
			vector<ColumnIndex> column_ids;
			column_ids.reserve(col_sel.size());
			for (auto col_sel_idx : col_sel) {
				auto entry = column_references.find(ColumnBinding(get.table_index, col_sel_idx));
				if (entry == column_references.end()) {
					throw InternalException("RemoveUnusedColumns - could not find referenced column");
				}
				ColumnIndex new_index(final_column_ids[col_sel_idx].GetPrimaryIndex(), entry->second.child_columns);
				column_ids.emplace_back(new_index);
			}
			if (column_ids.empty()) {
				// this generally means we are only interested in whether or not anything exists in the table (e.g.
				// EXISTS(SELECT * FROM tbl)) in this case, we just scan the row identifier column as it means we do not
				// need to read any of the columns
				column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
			}
			get.SetColumnIds(std::move(column_ids));

			if (get.function.filter_prune) {
				// Now set the projection cols by matching the "selection vector" that excludes filter columns
				// with the "selection vector" that includes filter columns
				idx_t col_idx = 0;
				get.projection_ids.clear();
				for (auto proj_sel_idx : proj_sel) {
					for (; col_idx < col_sel.size(); col_idx++) {
						if (proj_sel_idx == col_sel[col_idx]) {
							get.projection_ids.push_back(col_idx);
							break;
						}
					}
				}
			}
		}
		return;
	case LogicalOperatorType::LOGICAL_DISTINCT: {
		auto &distinct = op.Cast<LogicalDistinct>();
		if (distinct.distinct_type == DistinctType::DISTINCT_ON) {
			// distinct type references columns that need to be distinct on, so no
			// need to implicity reference everything.
			break;
		}
		// distinct, all projected columns are used for the DISTINCT computation
		// mark all columns as used and continue to the children
		// FIXME: DISTINCT with expression list does not implicitly reference everything
		everything_referenced = true;
		break;
	}
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE: {
		everything_referenced = true;
		break;
	}
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE: {
		// The outputs are the consuming child's, pruned by what the parents read. The definition is visited after it,
		// when the references have said which of its columns they read.
		auto table_index = op.Cast<LogicalMaterializedCTE>().table_index;
		auto &reads = (*cte_reads)[table_index];
		reads = CTEReads();
		reads.width = op.children[0]->GetColumnBindings().size();
		reads.read.assign(reads.width, false);
		VisitOperator(*op.children[1]);
		// A reference this pass did not reach reads columns it did not see.
		if (reads.visited == CountCTEReferences(*op.children[1], table_index)) {
			NarrowMaterializedCTE(op, reads);
		}
		RemoveUnusedColumns definition(*this, true);
		definition.VisitOperator(*op.children[0]);
		return;
	}
	case LogicalOperatorType::LOGICAL_CTE_REF: {
		// A reference to a CTE this pass does not narrow (a recursive one) reads all of it.
		auto &ref = op.Cast<LogicalCTERef>();
		auto entry = cte_reads->find(ref.cte_index);
		if (entry != cte_reads->end()) {
			auto &reads = entry->second;
			D_ASSERT(ref.chunk_types.size() == reads.width);
			reads.visited++;
			for (idx_t i = 0; i < reads.width; i++) {
				if (everything_referenced ||
				    column_references.find(ColumnBinding(ref.table_index, i)) != column_references.end()) {
					reads.read[i] = true;
				}
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_PIVOT: {
		everything_referenced = true;
		break;
	}
	case LogicalOperatorType::LOGICAL_DECIDE: {
        auto &decide = op.Cast<LogicalDecide>();
        // See LogicalDecide::EnumerateExpressions: the single authoritative list of
        // every expression the operator owns (including keyed decisions' keys and composed
        // MIN/MAX terms), shared with ColumnBindingResolver and every generic
        // LogicalOperatorVisitor pass instead of re-listed by hand here.
        decide.EnumerateExpressions([&](unique_ptr<Expression> *expr) { VisitExpression(expr); });
        everything_referenced = true;
        break;
	}
	case LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR: {
		if (!everything_referenced) {
			auto bindings = op.GetColumnBindings();
			vector<bool> referenced_outputs;
			referenced_outputs.reserve(bindings.size());
			for (auto &binding : bindings) {
				referenced_outputs.push_back(column_references.find(binding) != column_references.end());
			}
			op.Cast<LogicalExtensionOperator>().PruneUnusedOutputs(referenced_outputs);
		}
		break;
	}
	default:
		break;
	}
	LogicalOperatorVisitor::VisitOperatorExpressions(op);
	LogicalOperatorVisitor::VisitOperatorChildren(op);

	if (op.type == LogicalOperatorType::LOGICAL_ASOF_JOIN || op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN ||
	    op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		auto &comp_join = op.Cast<LogicalComparisonJoin>();
		// after removing duplicate columns we may have duplicate join conditions (if the join graph is cyclical)
		vector<JoinCondition> unique_conditions;
		for (auto &cond : comp_join.conditions) {
			bool found = false;
			for (auto &unique_cond : unique_conditions) {
				if (cond.comparison == unique_cond.comparison && cond.left->Equals(*unique_cond.left) &&
				    cond.right->Equals(*unique_cond.right)) {
					found = true;
					break;
				}
			}
			if (!found) {
				unique_conditions.push_back(std::move(cond));
			}
		}
		comp_join.conditions = std::move(unique_conditions);
	}
}

bool BaseColumnPruner::HandleStructExtractRecursive(Expression &expr, optional_ptr<BoundColumnRefExpression> &colref,
                                                    vector<idx_t> &indexes) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	if (function.function.name != "struct_extract_at" && function.function.name != "struct_extract" &&
	    function.function.name != "array_extract") {
		return false;
	}
	if (!function.bind_info) {
		return false;
	}
	if (function.children[0]->return_type.id() != LogicalTypeId::STRUCT) {
		return false;
	}
	auto &bind_data = function.bind_info->Cast<StructExtractBindData>();
	indexes.push_back(bind_data.index);
	// struct extract, check if left child is a bound column ref
	if (function.children[0]->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		// column reference - check if it is a struct
		auto &ref = function.children[0]->Cast<BoundColumnRefExpression>();
		if (ref.return_type.id() != LogicalTypeId::STRUCT) {
			return false;
		}
		colref = &ref;
		return true;
	}
	// not a column reference - try to handle this recursively
	if (!HandleStructExtractRecursive(*function.children[0], colref, indexes)) {
		return false;
	}
	return true;
}

bool BaseColumnPruner::HandleStructExtract(Expression &expr) {
	optional_ptr<BoundColumnRefExpression> colref;
	vector<idx_t> indexes;
	if (!HandleStructExtractRecursive(expr, colref, indexes)) {
		return false;
	}
	D_ASSERT(!indexes.empty());
	// construct the ColumnIndex
	ColumnIndex index = ColumnIndex(indexes[0]);
	for (idx_t i = 1; i < indexes.size(); i++) {
		ColumnIndex new_index(indexes[i]);
		new_index.AddChildIndex(std::move(index));
		index = std::move(new_index);
	}
	AddBinding(*colref, std::move(index));
	return true;
}

void MergeChildColumns(vector<ColumnIndex> &current_child_columns, ColumnIndex &new_child_column) {
	if (current_child_columns.empty()) {
		// there's already a reference to the full column - we can't extract only a subfield
		// skip struct projection pushdown
		return;
	}
	// if we are already extract sub-fields, add it (if it is not there yet)
	for (auto &binding : current_child_columns) {
		if (binding.GetPrimaryIndex() != new_child_column.GetPrimaryIndex()) {
			continue;
		}
		// found a match: sub-field is already projected
		// check if we have child columns
		auto &nested_child_columns = binding.GetChildIndexesMutable();
		if (!new_child_column.HasChildren()) {
			// new child is a reference to a full column - clear any existing bindings (if any)
			nested_child_columns.clear();
		} else {
			// new child has a sub-reference - merge recursively
			D_ASSERT(new_child_column.ChildIndexCount() == 1);
			MergeChildColumns(nested_child_columns, new_child_column.GetChildIndex(0));
		}
		return;
	}
	// this child column is not projected yet - add it in
	current_child_columns.push_back(std::move(new_child_column));
}

void BaseColumnPruner::AddBinding(BoundColumnRefExpression &col, ColumnIndex child_column) {
	auto entry = column_references.find(col.binding);
	if (entry == column_references.end()) {
		// column not referenced yet - add a binding to it entirely
		ReferencedColumn column;
		column.bindings.push_back(col);
		column.child_columns.push_back(std::move(child_column));
		column_references.insert(make_pair(col.binding, std::move(column)));
	} else {
		// column reference already exists - check add the binding
		auto &column = entry->second;
		column.bindings.push_back(col);

		MergeChildColumns(column.child_columns, child_column);
	}
}

void BaseColumnPruner::AddBinding(BoundColumnRefExpression &col) {
	auto entry = column_references.find(col.binding);
	if (entry == column_references.end()) {
		// column not referenced yet - add a binding to it entirely
		column_references[col.binding].bindings.push_back(col);
	} else {
		// column reference already exists - add the binding and clear any sub-references
		auto &column = entry->second;
		column.bindings.push_back(col);
		column.child_columns.clear();
	}
}

void BaseColumnPruner::VisitExpression(unique_ptr<Expression> *expression) {
	auto &expr = **expression;
	if (HandleStructExtract(expr)) {
		// already handled
		return;
	}
	// recurse
	LogicalOperatorVisitor::VisitExpression(expression);
}

unique_ptr<Expression> BaseColumnPruner::VisitReplace(BoundColumnRefExpression &expr,
                                                      unique_ptr<Expression> *expr_ptr) {
	// add a reference to the entire column
	AddBinding(expr);
	return nullptr;
}

unique_ptr<Expression> BaseColumnPruner::VisitReplace(BoundReferenceExpression &expr,
                                                      unique_ptr<Expression> *expr_ptr) {
	// BoundReferenceExpression should not be used here yet, they only belong in the physical plan
	throw InternalException("BoundReferenceExpression should not be used here yet!");
}

} // namespace duckdb
