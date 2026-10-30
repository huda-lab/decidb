//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/remove_unused_columns.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/column_index.hpp"

namespace duckdb {
class Binder;
class BoundColumnRefExpression;
class ClientContext;

struct ReferencedColumn {
	vector<reference<BoundColumnRefExpression>> bindings;
	vector<ColumnIndex> child_columns;
};

class BaseColumnPruner : public LogicalOperatorVisitor {
protected:
	//! The map of column references
	column_binding_map_t<ReferencedColumn> column_references;

protected:
	void VisitExpression(unique_ptr<Expression> *expression) override;

	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override;
	unique_ptr<Expression> VisitReplace(BoundReferenceExpression &expr, unique_ptr<Expression> *expr_ptr) override;

protected:
	//! Add a reference to the column in its entirey
	void AddBinding(BoundColumnRefExpression &col);
	//! Add a reference to a sub-section of the column
	void AddBinding(BoundColumnRefExpression &col, ColumnIndex child_column);
	//! Perform a replacement of the ColumnBinding, iterating over all the currently found column references and
	//! replacing the bindings
	void ReplaceBinding(ColumnBinding current_binding, ColumnBinding new_binding);

	bool HandleStructExtract(Expression &expr);

	bool HandleStructExtractRecursive(Expression &expr, optional_ptr<BoundColumnRefExpression> &colref,
	                                  vector<idx_t> &indexes);
};

//! The RemoveUnusedColumns optimizer traverses the logical operator tree and removes any columns that are not required.
//! A materialized CTE keeps only the columns its references read (projection pushdown across the CTE boundary, the
//! column twin of the CTE filter pusher): every reference takes the same narrowed layout, because a CTE scan reads the
//! materialized columns by position.
class RemoveUnusedColumns : public BaseColumnPruner {
public:
	RemoveUnusedColumns(Binder &binder, ClientContext &context, bool is_root = false);

	void VisitOperator(LogicalOperator &op) override;

private:
	//! What the pass learned of one materialized CTE: its width, the columns its references read, how many references
	//! it visited, and the columns it kept (empty when it kept all).
	struct CTEReads {
		idx_t width = 0;
		vector<bool> read;
		idx_t visited = 0;
		vector<idx_t> kept;
	};
	using cte_reads_map_t = unordered_map<idx_t, CTEReads>;

	//! A pass nested in `parent`'s, which shares what it learned of the CTEs.
	explicit RemoveUnusedColumns(RemoveUnusedColumns &parent, bool is_root = false);

	Binder &binder;
	ClientContext &context;
	//! Whether or not all the columns are referenced. This happens in the case of the root expression (because the
	//! output implicitly refers all the columns below it)
	bool everything_referenced;
	//! Set until the outermost call starts: that call narrows the CTE references once the whole plan is pruned.
	bool outermost;
	//! Shared by every pass nested in this one, keyed by the CTE's table index.
	shared_ptr<cte_reads_map_t> cte_reads;

	//! `child` under a projection of its `columns` (the first column when there are none, which is added to `columns`).
	unique_ptr<LogicalOperator> ProjectColumns(unique_ptr<LogicalOperator> child, vector<idx_t> &columns);
	//! Narrows the definition of the materialized CTE `op` to the columns its references read.
	void NarrowMaterializedCTE(LogicalOperator &op, CTEReads &reads);
	//! Gives every reference to a narrowed CTE the narrowed layout and re-points the plan's reads of it.
	void NarrowCTEReferences(LogicalOperator &plan);

private:
	template <class T>
	void ClearUnusedExpressions(vector<T> &list, idx_t table_idx, bool replace = true);
};
} // namespace duckdb
