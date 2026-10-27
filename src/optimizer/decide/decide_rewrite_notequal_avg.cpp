//===----------------------------------------------------------------------===//
//                         DecidB
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
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"

namespace duckdb {

void DecideOptimizer::RewriteNotEqual(LogicalDecide &decide) {
	if (!decide.decide_constraints) {
		return;
	}
	// Walk the bound constraint tree and find all COMPARE_NOTEQUAL expressions.
	// For each one, create an auxiliary BOOLEAN indicator variable.
	// The constraint expression itself is NOT modified — the physical operator
	// matches COMPARE_NOTEQUAL constraints with ne_clause_labels at execution time.
	FindNotEqualConstraints(*decide.decide_constraints, decide);
}

void DecideOptimizer::FindNotEqualConstraints(Expression &expr, LogicalDecide &decide) {
	ForEachConstraintLeaf(expr, [&](Expression &node) {
		if (node.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return;
		}
		auto &comp = node.Cast<BoundComparisonExpression>();
		if (comp.type != ExpressionType::COMPARE_NOTEQUAL) {
			return;
		}
		// The two rows a `<>` becomes hold a linear left side only. A product with a BOOL
		// decision is linear by now (RewriteBilinear runs before this pass); a product or a
		// power of decisions still here has no formulation, and dropping its terms would
		// change the answer.
		if (!DecideExpressionDegree(*comp.left, decide.decide_index).IsLinear()) {
			throw BinderException(comp, "DECIDE: <> over a product or a power of decisions is not supported yet: %s",
			                      RenderForPlan(comp, decide));
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
		idx_t clause_idx = decide.ne_clause_labels.size();
		auto clause_text = RenderForPlan(comp, decide);
		decide.ne_clause_labels.push_back(clause_text);
		AddRewriteAccount(decide, comp.alias, LogicalDecide::RewriteAccount::Kind::NOT_EQUAL, std::move(clause_text));
		AddDecideTag(comp.alias, string(NE_CLAUSE_TAG_PREFIX) + to_string(clause_idx) + "__");
	});
}

// ---------------------------------------------------------------------------
// AVG → SUM rewrite
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteAvgToSum(LogicalDecide &decide) {
	// Rewrites mutate in place, so each clause is detached, rewritten, and
	// reinstalled through the boundary rather than edited where it sits.
	if (decide.decide_constraints) {
		auto constraints = std::move(decide.decide_constraints);
		RewriteAvgInExpression(constraints, decide, string());
		AppendConstraint(decide, std::move(constraints));
	}
	if (decide.decide_objective) {
		auto objective = std::move(decide.decide_objective);
		RewriteAvgInExpression(objective, decide, OBJECTIVE_ROW_TAG);
		decide.SetObjective(optimizer.context, std::move(objective));
	}
}

void DecideOptimizer::RewriteAvgInExpression(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                             const string &source_alias) {
	if (!expr) {
		return;
	}
	const idx_t decide_index = decide.decide_index;
	auto clause_alias = DescendSourceAlias(*expr, source_alias);

	// Check if this node is an AVG aggregate — may be wrapped in a BOUND_CAST
	Expression *inner = expr.get();
	bool has_cast = false;
	if (inner->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		has_cast = true;
		inner = inner->Cast<BoundCastExpression>().child.get();
	}

	if (inner->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &agg = inner->Cast<BoundAggregateExpression>();
		// A decision-free AVG is left alone. The rewrite exists only to linearize a
		// decision-bearing AVG into `1/N * x_i` coefficients; a data-only AVG has
		// nothing to linearize, it is just a number the right-hand side evaluates.
		// Rewriting it is actively harmful there: BindAggregateFunction("sum", ...)
		// redeclares the node with SUM's type (HUGEINT where AVG is DOUBLE), and the
		// right-hand side must hand its value back to the surrounding expression,
		// which was bound against that declared type -- so the fractional part is
		// cast away. Leaving it as a real AVG keeps the round trip DOUBLE->DOUBLE.
		if (StringUtil::CIEquals(agg.function.name, "avg") && agg.children.size() == 1 &&
		    BoundExpressionReferencesDecide(*inner, decide_index)) {
			AddRewriteAccount(decide, clause_alias, LogicalDecide::RewriteAccount::Kind::AVG_SUM,
			                  RenderForPlan(agg, decide));
			// Replace AVG(expr) with SUM(expr)
			vector<unique_ptr<Expression>> sum_children;
			sum_children.push_back(agg.children[0]->Copy());
			auto new_sum = optimizer.BindAggregateFunction("sum", std::move(sum_children));
			if (agg.filter) {
				new_sum->Cast<BoundAggregateExpression>().filter = agg.filter->Copy();
			}

			// Tag so execution layer knows to apply AVG's row-count denominator.
			// For a single objective AVG this is optimization-equivalent to SUM,
			// but additive objective expressions can mix AVG with SUM or filtered
			// aggregates, so preserving the scale is required for correct semantics.
			// Carry the AVG's own tags across — a relation qualifier was stamped by
			// the binder and still names the relation the SUM de-duplicates by.
			new_sum->alias = agg.alias;
			AddDecideTag(new_sum->alias, AVG_REWRITE_TAG);

			if (has_cast) {
				// Preserve the cast wrapper — update its child
				auto &cast_expr = expr->Cast<BoundCastExpression>();
				cast_expr.child = std::move(new_sum);
			} else {
				expr = std::move(new_sum);
			}
			return;
		}
	}

	// Recurse into children
	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { RewriteAvgInExpression(child, decide, clause_alias); });
}

} // namespace duckdb
