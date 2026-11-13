#include "duckdb/optimizer/decide/direct/direct_constant_propagation.hpp"

#include "duckdb/optimizer/constant_columns.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

DirectConstantPropagation::DirectConstantPropagation(ExpressionRewriter &rewriter_p) : rewriter(rewriter_p) {
}

void DirectConstantPropagation::VisitOperator(LogicalOperator &op) {
	VisitOperatorChildren(op);
	VisitOperatorExpressions(op);
	rewriter.RewriteOperator(op);
	ForgetPadded(op);
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op.Cast<LogicalProjection>();
		for (idx_t i = 0; i < projection.expressions.size(); i++) {
			Value value;
			if (TryGetConstantValue(*projection.expressions[i], Lookup(), value)) {
				constants.emplace(ColumnBinding(projection.table_index, i), std::move(value));
			}
		}
	}
}

void DirectConstantPropagation::VisitExpression(unique_ptr<Expression> *expression) {
	// The expression itself stays: its owner (a projection, a join, an aggregate) names it by its binding.
	Substitute(**expression);
}

void DirectConstantPropagation::Substitute(Expression &expr) {
	SubstituteConstantColumns(expr, Lookup());
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { Substitute(child); });
}

void DirectConstantPropagation::ForgetPadded(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN: {
		// RIGHT and FULL pad the left, LEFT, FULL and SINGLE pad the right (propagate_join.cpp)
		auto join_type = op.Cast<LogicalJoin>().join_type;
		if (IsRightOuterJoin(join_type)) {
			Forget(*op.children[0]);
		}
		if (IsLeftOuterJoin(join_type) || join_type == JoinType::SINGLE) {
			Forget(*op.children[1]);
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_POSITIONAL_JOIN:
		// the shorter side is padded
		Forget(*op.children[0]);
		Forget(*op.children[1]);
		break;
	default:
		break;
	}
}

void DirectConstantPropagation::Forget(LogicalOperator &child) {
	for (auto &binding : child.GetColumnBindings()) {
		constants.erase(binding);
	}
}

} // namespace duckdb
