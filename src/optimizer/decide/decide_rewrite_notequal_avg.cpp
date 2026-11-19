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
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"

namespace duckdb {

void DecideOptimizer::RewriteNotEqual(LogicalDecide &decide) {
	// Walk the bound constraint tree and find all COMPARE_NOTEQUAL expressions.
	// For each one, create an auxiliary BOOLEAN indicator variable.
	// The constraint expression itself is NOT modified — the physical operator
	// matches COMPARE_NOTEQUAL constraints with ne_clause_labels at execution time.
	FindNotEqualConstraints(*decide.decide_constraints, decide);
}

void DecideOptimizer::FindNotEqualConstraints(Expression &expr, LogicalDecide &decide) {
	DecideTermSplitter splitter(optimizer.context, decide);
	ForEachConstraintLeaf(expr, [&](Expression &node) {
		if (node.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return;
		}
		auto &comp = node.Cast<BoundComparisonExpression>();
		if (comp.type != ExpressionType::COMPARE_NOTEQUAL) {
			return;
		}
		// The two rows a `<>` becomes hold a linear left side only. A product with a BOOL decision is linear through
		// its F helper (RewriteBilinear tags it before this pass); a product or a power of decisions still here has no
		// formulation, and dropping its terms would change the answer.
		if (!splitter.IsLinear(*comp.left)) {
			throw NotImplementedException(comp, "<> over a product or a power of decisions is not supported");
		}
		// No indicator variable. Whether a `<>` even HAS a disjunction to switch is a
		// question about evaluated data — a range that lies wholly on one side of `K`
		// collapses to a plain inequality — and an aggregate spelling needs one binary
		// per GROUP rather than per row. Neither is knowable here, so allocating a
		// row-scoped binary per data row now meant allocating one for every clause
		// that turned out to need none.
		//
		// What stage 05 owns is the marking: which clause this is, and the text to
		// call it in a diagnosis, spelled as the plan prints the clause. The clause index
		// rides the tag; stage 06 allocates the binaries for the disjunctions it actually
		// emits, and labels them here.
		idx_t clause_idx = decide.ne_clause_sources.size();
		decide.ne_clause_sources.push_back(SourceClauseOf(comp, decide));
		AddRewriteAccount(decide, comp.alias, LogicalDecide::RewriteAccount::Kind::NOT_EQUAL,
		                  RenderForPlan(comp, decide));
		AddDecideTag(comp.alias, string(NE_CLAUSE_TAG_PREFIX) + to_string(clause_idx) + "__");
	});
}

// ---------------------------------------------------------------------------
// AVG → SUM rewrite
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteAvgToSum(LogicalDecide &decide) {
	// Rewrites mutate in place, so each clause is detached, rewritten, and
	// reinstalled through the boundary rather than edited where it sits.
	auto constraints = std::move(decide.decide_constraints);
	RewriteAvgInExpression(constraints, decide, string());
	decide.AddConstraint(optimizer.context, std::move(constraints));
	if (decide.decide_objective) {
		auto objective = std::move(decide.decide_objective);
		RewriteAvgInExpression(objective, decide, OBJECTIVE_ROW_TAG);
		decide.SetObjective(optimizer.context, std::move(objective));
	}
}

void DecideOptimizer::RewriteAvgInExpression(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                             const string &source_alias) {
	idx_t definition_aux;
	if (TryParseDecideIndexTag(expr->GetAlias(), DEFINITION_FUNCTION_TAG_PREFIX, definition_aux)) {
		return; // AVG syntax inside this body belongs to the tree F node's definition expression
	}
	auto clause_alias = DescendSourceAlias(*expr, source_alias);

	if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &agg = expr->Cast<BoundAggregateExpression>();
		// The rewrite linearizes an AVG over decisions. One over data is a value execution computes: the bound's is a
		// placeholder column already (RewriteRhsAggregates), and a frame's marker or a reducer reading a frame stays an
		// aggregate until the frames are lowered (MakeFrameAggregate).
		if (StringUtil::CIEquals(agg.function.name, "avg") &&
		    BoundExpressionReferencesDecide(agg, decide.decide_index)) {
			AddRewriteAccount(decide, clause_alias, LogicalDecide::RewriteAccount::Kind::AVG_SUM,
			                  RenderForPlan(agg, decide));
			auto new_sum = BindSumOver(optimizer, agg.children[0]->Copy(), agg.filter.get());

			// Tag so execution layer knows to apply AVG's row-count denominator.
			// For a single objective AVG this is optimization-equivalent to SUM,
			// but additive objective expressions can mix AVG with SUM or filtered
			// aggregates, so preserving the scale is required for correct semantics.
			// Carry the AVG's own tags across — a relation qualifier was stamped by
			// the binder and still names the relation the SUM de-duplicates by.
			new_sum->alias = agg.alias;
			AddDecideTag(new_sum->alias, AVG_REWRITE_TAG);

			expr = std::move(new_sum);
			return;
		}
	}

	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { RewriteAvgInExpression(child, decide, clause_alias); });
}

} // namespace duckdb
