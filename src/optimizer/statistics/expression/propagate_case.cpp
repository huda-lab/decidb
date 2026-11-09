#include "duckdb/optimizer/rule/case_simplification.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"

namespace duckdb {

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateExpression(BoundCaseExpression &bound_case,
                                                                     unique_ptr<Expression> &expr_ptr) {
	// propagate in the checks first: a check the statistics prove constant folds the CASE, as the conjunction and
	// comparison handlers fold what they prove, and the branches it drops need no propagation
	for (auto &case_check : bound_case.case_checks) {
		PropagateExpression(case_check.when_expr);
	}
	auto folded = CaseSimplificationRule::Simplify(context, bound_case);
	if (folded) {
		// no check is left: the CASE is its ELSE
		expr_ptr = std::move(folded);
		return PropagateExpression(expr_ptr);
	}
	auto result_stats = PropagateExpression(bound_case.else_expr);
	for (auto &case_check : bound_case.case_checks) {
		auto then_stats = PropagateExpression(case_check.then_expr);
		if (!then_stats) {
			result_stats.reset();
		} else if (result_stats) {
			result_stats->Merge(*then_stats);
		}
	}
	return result_stats;
}

} // namespace duckdb
