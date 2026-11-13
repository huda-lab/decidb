#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"

namespace duckdb {

class ExpressionRewriter;

//! A rule builds its plan from projection stages, so a constant it lays out as a column (N1's `q` of a constant slope,
//! a `true` flag, A1's floor) reaches every projection above it as a column reference, where DuckDB's rewriter, which
//! folds within one expression, cannot see it. This pass carries such a constant to the expressions that read it,
//! then simplifies them: constant propagation with peephole simplification in one bottom-up pass over a tree, the
//! order a sparse conditional constant propagation followed by an instruction simplifier gives, and a tree needs no
//! second round. It runs over the subtree a rule installs, in place of the rewriter alone.
//!
//! At each operator, children first: every column reference that reads a known constant column becomes the constant,
//! except a reference that is the whole expression (a projection's output, a join key, a group, a filter's predicate
//! stays a column, so no binding changes); the optimizer's own rewriter then runs over the operator's expressions; a
//! projection records each of its columns that is a constant or a whole-expression reference to a known one. A join
//! that pads a side with NULL forgets that side's constants. Every other operator passes its bindings through: a
//! binding names one column, and a filter, window, cross product or order keeps its rows' values. An aggregate and a
//! CTE reference create new bindings, so no constant crosses them. The pass adds only constants, so it changes no
//! output binding or type.
class DirectConstantPropagation : public LogicalOperatorVisitor {
public:
	//! `rewriter` is the optimizer's own, with its rule list filled.
	explicit DirectConstantPropagation(ExpressionRewriter &rewriter);

	void VisitOperator(LogicalOperator &op) override;
	void VisitExpression(unique_ptr<Expression> *expression) override;

private:
	//! Replace the constant columns read inside `expr`, where `expr` itself is not replaced.
	void Substitute(Expression &expr);
	//! Forget the columns of the children of `op` that it can pad with NULL.
	void ForgetPadded(LogicalOperator &op);
	void Forget(LogicalOperator &child);
	//! The value of a known constant column, as the callable the shared constant-column functions take
	//! (constant_columns.hpp).
	auto Lookup() const {
		return [this](ColumnBinding binding, Value &value) {
			auto found = constants.find(binding);
			if (found == constants.end()) {
				return false;
			}
			value = found->second;
			return true;
		};
	}

	ExpressionRewriter &rewriter;
	//! The columns that hold one value on every row, with the value.
	column_binding_map_t<Value> constants;
};

} // namespace duckdb
