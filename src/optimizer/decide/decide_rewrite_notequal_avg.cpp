//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/optimizer/decide/decide_rewrite_notequal_avg.cpp
//
// DECIDE `<>` and AVG rewrites. See decide_optimizer.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/planner/decide/decide_cast_policy.hpp"

#include <cstdlib>
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/decide_solver_gate.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"

namespace duckdb {

void DecideOptimizer::RewriteNotEqual(LogicalDecide &decide) {
	// Walk the bound clauses and find all COMPARE_NOTEQUAL expressions.
	// The constraint expression itself is NOT modified — the physical operator
	// reads each COMPARE_NOTEQUAL constraint at execution time.
	DecideTermSplitter splitter(optimizer.context, decide);
	decide.EnumerateBodies([&](DecideOwnedBody &body) {
		if (body.owner != DecideOwnedBody::Owner::CLAUSE ||
		    body.expr->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return;
		}
		auto &comp = body.expr->Cast<BoundComparisonExpression>();
		if (comp.type != ExpressionType::COMPARE_NOTEQUAL) {
			return;
		}
		// The two rows a `<>` becomes hold a linear left side only. A product with a BOOL decision is linear through
		// its F helper (RewriteBilinear keeps it before this pass); a product or a power of decisions still here has no
		// formulation, and dropping its terms would change the answer.
		RequireLinearOperand(splitter, *comp.left, comp, "<>");
		// No indicator variable. Whether a `<>` even HAS a disjunction to switch is a
		// question about evaluated data — a range that lies wholly on one side of `K`
		// collapses to a plain inequality — and an aggregate spelling needs one binary
		// per GROUP rather than per row. Neither is knowable here, so allocating a
		// row-scoped binary per data row now meant allocating one for every clause
		// that turned out to need none.
		//
		// What stage 05 owns is the account: the text to call the clause in a diagnosis,
		// spelled as the plan prints it. Stage 06 allocates the binaries for the
		// disjunctions it actually emits, and labels them with the clause.
		AddRewriteAccount(decide, body.Origin().source_clause_id, LogicalDecide::RewriteAccount::Kind::NOT_EQUAL,
		                  RenderForPlan(comp, decide));
	});
}

// ---------------------------------------------------------------------------
// AVG's account
// ---------------------------------------------------------------------------

//! Records the account of each AVG over decisions under `expr`. One over data is a value execution computes, and one
//! inside a definition's body belongs to that definition.
static void AccountAvgs(const Expression &expr, LogicalDecide &decide, const DecideRowOrigin &origin) {
	if (DecideDefinitionOf(expr).Is(DecideHelperKind::DEFINITION)) {
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER &&
	    expr.Cast<BoundDecideReducerExpression>().kind == DecideReducerKind::AVG &&
	    BoundExpressionReferencesDecide(expr, decide.decide_index)) {
		AddRewriteAccount(decide, origin.source_clause_id, LogicalDecide::RewriteAccount::Kind::AVG_SUM,
		                  RenderForPlan(expr, decide));
		return;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { AccountAvgs(child, decide, origin); });
}

void DecideOptimizer::AccountAvg(LogicalDecide &decide) {
	decide.EnumerateBodies([&](DecideOwnedBody &body) {
		if (body.owner == DecideOwnedBody::Owner::CLAUSE || body.owner == DecideOwnedBody::Owner::OBJECTIVE) {
			AccountAvgs(*body.expr, decide, body.Origin());
		}
	});
}

} // namespace duckdb
