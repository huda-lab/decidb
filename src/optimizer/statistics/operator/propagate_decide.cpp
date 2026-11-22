#include "duckdb/planner/expression/bound_decide_null_test_expression.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

void StatisticsPropagator::FoldSkipNullTests(unique_ptr<Expression> &expr) {
	if (expr->GetExpressionClass() != ExpressionClass::BOUND_DECIDE_NULL_TEST) {
		ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { FoldSkipNullTests(child); });
		return;
	}
	// The test is read for its verdict alone; the value it tests stays as written.
	auto &test = expr->Cast<BoundDecideNullTestExpression>().test;
	auto verdict = test->Copy();
	PropagateExpression(verdict);
	if (verdict->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		test = std::move(verdict);
	}
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalDecide &decide,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	auto result = PropagateChildren(decide, node_ptr);
	// A NULL test DecideCanonicalizer::SkipNullData added on a value the statistics prove never NULL keeps every row:
	// it folds to TRUE, as a filter's `IS NOT NULL` does, and stage 08 reads no filter there. The tests read the
	// input's columns only, never a decision the operator produces.
	decide.EnumerateExpressions([&](unique_ptr<Expression> *expr) { FoldSkipNullTests(*expr); });
	return result;
}

} // namespace duckdb
