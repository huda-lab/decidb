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
		// its F helper (RewriteBilinear tags it before this pass); a product or a power of decisions still here has no
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
// AVG → SUM rewrite
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteAvgToSum(LogicalDecide &decide) {
	// Rewrites mutate in place, so a clause and the objective are rewritten where they sit, and one an AVG was
	// rewritten in goes back through the canonicalizing boundary.
	vector<BoundDecideClause *> rewritten;
	decide.EnumerateBodies([&](DecideOwnedBody &body) {
		if ((body.owner == DecideOwnedBody::Owner::CLAUSE || body.owner == DecideOwnedBody::Owner::OBJECTIVE) &&
		    RewriteAvgInExpression(body.expr, decide, body.Origin())) {
			rewritten.push_back(body.clause.get());
		}
	});
	for (auto clause : rewritten) {
		if (clause == &decide.bound.objective) {
			decide.SetObjective(optimizer.context, std::move(decide.bound.objective));
		} else {
			decide.CanonicalizeClause(optimizer.context, *clause);
		}
	}
}

bool DecideOptimizer::RewriteAvgInExpression(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                             const DecideRowOrigin &origin) {
	idx_t definition_aux;
	if (TryParseDecideIndexTag(expr->GetAlias(), DEFINITION_FUNCTION_TAG_PREFIX, definition_aux)) {
		return false; // AVG syntax inside this body belongs to the tree F node's definition expression
	}
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER) {
		auto &agg = expr->Cast<BoundDecideReducerExpression>();
		// The rewrite linearizes an AVG over decisions. One over data is a value execution computes: the bound's is a
		// placeholder column already (RewriteRhsAggregates), and a frame's marker or a reducer reading a frame stays an
		// aggregate until the frames are lowered (MakeFrameAggregate).
		if (StringUtil::CIEquals(agg.function->name, "avg") &&
		    BoundExpressionReferencesDecide(agg, decide.decide_index)) {
			AddRewriteAccount(decide, origin.source_clause_id, LogicalDecide::RewriteAccount::Kind::AVG_SUM,
			                  RenderForPlan(agg, decide));
			auto new_sum = BindSumOver(optimizer, agg.children[0]->Copy(), agg.filter.get());

			// The count scale tells the execution layer to apply AVG's row-count denominator.
			// For a single objective AVG this is optimization-equivalent to SUM,
			// but additive objective expressions can mix AVG with SUM or filtered
			// aggregates, so preserving the scale is required for correct semantics.
			// The SUM keeps the AVG's name and facts: its `per` still names the classes the SUM reads one row of.
			new_sum->alias = agg.alias;
			new_sum->Cast<BoundDecideReducerExpression>().CopyFacts(agg);
			new_sum->Cast<BoundDecideReducerExpression>().count_scale = true;

			expr = std::move(new_sum);
			return true;
		}
	}

	bool rewritten = false;
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		rewritten = RewriteAvgInExpression(child, decide, origin) || rewritten;
	});
	return rewritten;
}

} // namespace duckdb
