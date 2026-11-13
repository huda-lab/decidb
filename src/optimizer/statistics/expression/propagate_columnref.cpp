#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateExpression(BoundColumnRefExpression &colref,
                                                                     unique_ptr<Expression> &expr_ptr) {
	auto stats = statistics_map.find(colref.binding);
	if (stats == statistics_map.end()) {
		return nullptr;
	}
	return stats->second->ToUnique();
}

bool StatisticsPropagator::TryGetConstantColumn(ColumnBinding binding, Value &value) {
	auto constant = constant_columns.find(binding);
	auto stats = statistics_map.find(binding);
	if (constant == constant_columns.end() || stats == statistics_map.end()) {
		return false;
	}
	// a non-NULL value holds only while the column cannot hold NULL, and a NULL only while it holds nothing else
	if (constant->second.IsNull() ? stats->second->CanHaveNoNull() : stats->second->CanHaveNull()) {
		return false;
	}
	value = constant->second;
	return true;
}

void StatisticsPropagator::RecordConstantColumn(ColumnBinding binding, const Expression &expr) {
	Value value;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		constant_columns.emplace(binding, expr.Cast<BoundConstantExpression>().value);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	           TryGetConstantColumn(expr.Cast<BoundColumnRefExpression>().binding, value)) {
		constant_columns.emplace(binding, std::move(value));
	}
}

void StatisticsPropagator::SubstituteConstantColumns(Expression &expr) {
	ExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<Expression> &child) {
		Value value;
		if (child->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
		    TryGetConstantColumn(child->Cast<BoundColumnRefExpression>().binding, value)) {
			child = make_uniq<BoundConstantExpression>(std::move(value));
		}
	});
}

} // namespace duckdb
