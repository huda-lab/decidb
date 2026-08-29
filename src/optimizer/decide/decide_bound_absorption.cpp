//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/optimizer/decide/decide_bound_absorption.cpp
//
// DECIDE bound absorption: fold a literal bound into a column box. See decide_optimizer.cpp.
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
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
// Bound absorption: a bound, not a row
// ---------------------------------------------------------------------------
//
// `x <= 10` is one fact about a column, so it belongs in that column's box rather
// than in `num_rows` identical model rows. Choosing between the two is a
// formulation decision, which is why it lives here and not in physical execution.
//
// The pass reads a comparison, a decision variable and a foldable literal. It never
// touches data, so it needs types, not rows.

//! One decision variable, resolved, plus the type facts absorption needs about it.
//! The comparison and BETWEEN arms ask exactly the same questions, so they ask once.
struct AbsorptionTarget {
	LogicalDecide *decide = nullptr;
	idx_t var_idx = DConstants::INVALID_INDEX;
	bool is_integer = false;
	bool is_boolean = false;

	//! A bound that merely restates a BOOLEAN's intrinsic [0,1] box is not a loosenable
	//! parameter: the domain is applied to the solver column directly and never
	//! synthesized as a constraint, so such a bound only exists because the user wrote
	//! it redundantly. A genuine pin (`x <= 0`, `x >= 1`, `x = 1`) does tighten the box
	//! and is recorded — erasing those made the elastic model diverge from the query.
	bool ShouldRecord(char sense, double k) const {
		if (!is_boolean) {
			return true;
		}
		if (sense == '<' && k >= 1.0) {
			return false;
		}
		if (sense == '>' && k <= 0.0) {
			return false;
		}
		return true;
	}

	//! Tighten one side of the box and record the bound, in the one order that is always
	//! correct: tighten unconditionally (a BOOLEAN restatement is a harmless no-op against
	//! the intrinsic box), record only when ShouldRecord agrees.
	void Absorb(char sense, double k, bool strict, double typed_k, idx_t source_clause_id,
	            idx_t removal_group_id) const {
		auto &lower = decide->absorbed_lower_bounds[var_idx];
		auto &upper = decide->absorbed_upper_bounds[var_idx];
		if (sense != '>') {
			upper = std::min(upper, k); // '<' and '='
		}
		if (sense != '<') {
			lower = std::max(lower, k); // '>' and '='
		}
		if (ShouldRecord(sense, k)) {
			decide->user_absorbed_bounds.push_back(
			    {var_idx, sense, k, strict, typed_k, source_clause_id, removal_group_id});
		}
	}
};

//! Resolve `expr` to a decision variable. Fails for anything that is not a bare
//! decision reference under casts — a multi-variable LHS (`x - 3*z_1 - 5*z_2 = 0`,
//! the IN linking row) is a genuine relation, not a bound.
static bool TryMatchAbsorptionTarget(const Expression &expr, LogicalDecide &decide, AbsorptionTarget &out) {
	auto *unwrapped = UnwrapDecideCasts(const_cast<Expression &>(expr), decide.decide_index);
	if (unwrapped->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &colref = unwrapped->Cast<BoundColumnRefExpression>();
	for (idx_t i = 0; i < decide.decide_variables.size(); i++) {
		auto &decide_var = decide.decide_variables[i]->Cast<BoundColumnRefExpression>();
		if (colref.binding != decide_var.binding) {
			continue;
		}
		auto type_id = decide.decide_variables[i]->return_type.id();
		out.decide = &decide;
		out.var_idx = i;
		out.is_integer = (type_id == LogicalTypeId::INTEGER || type_id == LogicalTypeId::BIGINT);
		out.is_boolean = i < decide.is_boolean_var.size() && decide.is_boolean_var[i];
		return true;
	}
	return false;
}

void DecideOptimizer::AbsorbBoundsInExpression(Expression &expr, LogicalDecide &decide) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		// PER: only the constraint (child 0) carries bounds; the grouping columns do not.
		if (IsPerConstraintWrapper(conj) && conj.children.size() >= 2) {
			AbsorbBoundsInExpression(*conj.children[0], decide);
			break;
		}
		// WHEN: conditional per-row constraints must NOT contribute to a global bound.
		// `x <= 0 WHEN c` does not mean `x <= 0` everywhere. This is why the pass keeps
		// its own descent rather than using ForEachConstraintLeaf -- it asks the shared
		// predicate what a wrapper is, then deliberately answers differently.
		if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			break;
		}
		for (auto &child : conj.children) {
			AbsorbBoundsInExpression(*child, decide);
		}
		break;
	}

	case ExpressionClass::BOUND_COMPARISON: {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		idx_t source_clause_id = DConstants::INVALID_INDEX;
		TryParseSourceClauseTag(comp.GetAlias(), source_clause_id);
		idx_t removal_group_id = DConstants::INVALID_INDEX;
		TryParseRemovalGroupTag(comp.GetAlias(), removal_group_id);

		AbsorptionTarget target;
		if (!TryMatchAbsorptionTarget(*comp.left, decide, target)) {
			break;
		}

		// A non-finite bound is left for the constraint path, which already reads it the
		// way the solver does: `x <= +inf` is no bound, `x >= +inf` has no solution, and
		// NaN is an error naming the arithmetic. Absorbing it instead would write it into
		// the column box, where min/max against the ±1e30 sentinels keep an upper bound
		// but not a lower one — the same infinity silently vanishing in one direction and
		// reaching the model validator as an internal error in the other.
		double k;
		if (!IsCastWrappedConstant(*comp.right) ||
		    !TryEvaluateFoldableDouble(optimizer.context, *comp.right, k) || !std::isfinite(k)) {
			break;
		}

		// `=` intersects both sides rather than assigning, so `x = 5 AND x = 10` inverts
		// the box and the conflict is caught instead of resolving to whichever was
		// written last. A strict `<` / `>` normalizes into the bound for an integer
		// (`x < 10` is `x <= 9`), carrying the user's literal as `typed_k` so the
		// diagnosis re-quotes `< 10`. A REAL has no such normalization, so it declines
		// and the constraint path rejects it with a message naming the clause.
		bool absorbed = true;
		switch (comp.type) {
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			target.Absorb('<', k, false, 0.0, source_clause_id, removal_group_id);
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			target.Absorb('>', k, false, 0.0, source_clause_id, removal_group_id);
			break;
		case ExpressionType::COMPARE_EQUAL:
			target.Absorb('=', k, false, 0.0, source_clause_id, removal_group_id);
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			absorbed = target.is_integer;
			if (absorbed) {
				target.Absorb('<', k - 1.0, true, k, source_clause_id, removal_group_id);
			}
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			absorbed = target.is_integer;
			if (absorbed) {
				target.Absorb('>', k + 1.0, true, k, source_clause_id, removal_group_id);
			}
			break;
		default:
			absorbed = false;
			break;
		}

		if (absorbed) {
			// The comparison stays in the tree so EXPLAIN keeps rendering what the user
			// wrote; the tag is what stops it also becoming a model row.
			AddDecideTag(comp.alias, ABSORBED_BOUND_TAG);
		}
		break;
	}

	case ExpressionClass::BOUND_BETWEEN: {
		auto &between = expr.Cast<BoundBetweenExpression>();
		AbsorptionTarget target;
		if (!TryMatchAbsorptionTarget(*between.input, decide, target)) {
			break;
		}

		auto ExtractBound = [&](const Expression &e) -> double {
			double value;
			return IsCastWrappedConstant(e) && TryEvaluateFoldableDouble(optimizer.context, e, value)
			           ? value
			           : std::numeric_limits<double>::quiet_NaN();
		};

		// Each finite side is recorded as its own spec so the infeasible diagnosis
		// loosens BETWEEN uniformly with the other simple bounds. A strict side is
		// integer-normalized like the comparison arm, carrying the user's typed literal
		// so the diagnosis re-quotes `> a` rather than the normalized `>= a+1`.
		double lo = ExtractBound(*between.lower);
		if (!std::isnan(lo)) {
			bool strict = !between.lower_inclusive && target.is_integer;
			target.Absorb('>', strict ? lo + 1.0 : lo, strict, lo, DConstants::INVALID_INDEX,
			              DConstants::INVALID_INDEX);
		}
		double hi = ExtractBound(*between.upper);
		if (!std::isnan(hi)) {
			bool strict = !between.upper_inclusive && target.is_integer;
			target.Absorb('<', strict ? hi - 1.0 : hi, strict, hi, DConstants::INVALID_INDEX,
			              DConstants::INVALID_INDEX);
		}
		break;
	}

	case ExpressionClass::BOUND_CONSTANT:
		// Type declarations and rewrite placeholders (`TRUE`) carry no bound.
		break;

	default:
		break;
	}
}

void DecideOptimizer::AbsorbVariableBounds(LogicalDecide &decide) {
	// Sized here rather than at construction so every auxiliary variable created by the
	// preceding passes is already counted.
	idx_t num_decide_vars = decide.decide_variables.size();
	decide.absorbed_lower_bounds.assign(num_decide_vars, LogicalDecide::ABSORBED_LOWER_UNSET);
	decide.absorbed_upper_bounds.assign(num_decide_vars, 1e30);

	// Seed a BOOLEAN's intrinsic ceiling here rather than leaving every variable at
	// 1e30 and repairing it at model-build time. The box travels to stage 06 as
	// `SolverInput::upper_bounds`, and every Big-M derivation reads it through
	// `DecideRowTermRange`, which treats `>= 1e20` as unbounded: a declared BOOL with no
	// written upper bound therefore looked unbounded to all of them and collapsed to the
	// fallback constant. `SUM(x) <> 2` over four BOOLs took M = 1000000 where the same
	// query spelled `x(INT) ... x <= 1` took 7.
	//
	// The ceiling is a property of the declared type, known here, and it is rigid: the
	// elastic engine resets BOOLEAN columns only within [0,1], so no diagnosis opens it.
	// That also lets the `<>` range collapse see a BOOL's upper side, which it could not
	// before. Absorption only ever narrows (`std::min`), and a user restatement like
	// `x <= 1` is already treated as a harmless no-op against the intrinsic box, so
	// seeding it changes nothing about how a written bound is recorded.
	for (idx_t i = 0; i < num_decide_vars; i++) {
		if (i < decide.is_boolean_var.size() && decide.is_boolean_var[i]) {
			decide.absorbed_upper_bounds[i] = 1.0;
		}
	}

	if (decide.decide_constraints) {
		AbsorbBoundsInExpression(*decide.decide_constraints, decide);
	}
}

} // namespace duckdb
