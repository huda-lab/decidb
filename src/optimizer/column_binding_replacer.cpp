#include "duckdb/optimizer/column_binding_replacer.hpp"

#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"

namespace duckdb {

ReplacementBinding::ReplacementBinding(ColumnBinding old_binding, ColumnBinding new_binding)
    : old_binding(old_binding), new_binding(new_binding), replace_type(false) {
}

ReplacementBinding::ReplacementBinding(ColumnBinding old_binding, ColumnBinding new_binding, LogicalType new_type)
    : old_binding(old_binding), new_binding(new_binding), replace_type(true), new_type(std::move(new_type)) {
}

ColumnBindingReplacer::ColumnBindingReplacer() {
}

void ColumnBindingReplacer::VisitOperator(LogicalOperator &op) {
	if (stop_operator && stop_operator.get() == &op) {
		return;
	}
	VisitOperatorChildren(op);
	if (op.type == LogicalOperatorType::LOGICAL_DECIDE) {
		// LogicalDecide's expressions (constraints, objective, composed MIN/MAX terms,
		// entity keys, ...) live outside the base op.expressions that the generic
		// VisitOperatorExpressions walks -- see LogicalDecide::EnumerateExpressions, the
		// single authoritative field list this shares with ColumnBindingResolver. Without
		// this, a rewrite that renumbers child bindings below a DECIDE (e.g.
		// CompressedMaterialization inserting compress/decompress projections under a
		// join it wraps) leaves DECIDE's own references stale, since nothing else here
		// knows to update them.
		op.Cast<LogicalDecide>().EnumerateExpressions([&](unique_ptr<Expression> *expr) { VisitExpression(expr); });
		return;
	}
	VisitOperatorExpressions(op);
}

void ColumnBindingReplacer::VisitExpression(unique_ptr<Expression> *expression) {
	auto &expr = *expression;
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &bound_column_ref = expr->Cast<BoundColumnRefExpression>();
		for (const auto &replace_binding : replacement_bindings) {
			if (bound_column_ref.binding == replace_binding.old_binding) {
				bound_column_ref.binding = replace_binding.new_binding;
				if (replace_binding.replace_type) {
					bound_column_ref.return_type = replace_binding.new_type;
				}
			}
		}
	}

	VisitExpressionChildren(**expression);
}

} // namespace duckdb
