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

#include <cmath>
#include <cstdlib>
#include <limits>
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/decide_solver_gate.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
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
// The pass reads a comparison, a decision variable and a bound known before the query
// runs. It never touches data, so it needs types, not rows.
//
// A bound is read by value, not by spelling (D41): `x - 1 >= -3`, `2 * x >= -4`,
// `-x <= 2`, `2 * (x - 1) >= -6` and `x >= -2` are one fact, the column bound `x >= -2`,
// and each makes `x` signed the same way. So the decision may stand under any affine
// spelling whose constants are known before the query runs, and the other side may be
// any expression that folds to a number then. A side that is data -- a column, a
// subquery -- is not a bound and stays a row.

//! One decision variable, resolved, plus the type facts absorption needs about it, and
//! the affine spelling it stands in: the comparison's side is `scale * x + offset`
//! (`2 * x`: 2 and 0; `-x`: -1 and 0; `2 * (x - 1)`: 2 and -2).
struct AbsorptionTarget {
	LogicalDecide *decide = nullptr;
	idx_t var_idx = DConstants::INVALID_INDEX;
	bool is_integer = false;
	bool is_boolean = false;
	double scale = 1.0;
	double offset = 0.0;

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
	//! the intrinsic box), record only when ShouldRecord agrees. Returns whether it
	//! recorded: only a recorded bound is loosened by a diagnosis.
	bool Absorb(char sense, double k, bool strict, double typed_k, idx_t source_clause_id,
	            idx_t removal_group_id) const {
		auto &lower = decide->absorbed_lower_bounds[var_idx];
		auto &upper = decide->absorbed_upper_bounds[var_idx];
		if (sense != '>') {
			upper = std::min(upper, k); // '<' and '='
		}
		if (sense != '<') {
			lower = std::max(lower, k); // '>' and '='
		}
		if (!ShouldRecord(sense, k)) {
			return false;
		}
		decide->user_absorbed_bounds.push_back({var_idx, sense, k, strict, typed_k, source_clause_id, removal_group_id});
		return true;
	}
};

//! Read `expr` as the affine spelling `scale * e + offset` of the term `e` beneath its
//! factors and shifts -- `c * e`, `e * c`, `-e`, `e / c`, `e + c`, `c + e`, `e - c` and
//! `c - e`, with `c` a finite number known before the query runs and a factor nonzero --
//! and return `e`, under the binder's casts. A constant that is data, a subquery, or not
//! finite is not peeled: the term is then no bound, and the caller declines on its shape.
//! Canonicalization moves a top-level shift to the other side already; one under a
//! factor (`2 * (x - 1)`) it leaves in place, which is what the shifts are here for.
static const Expression *PeelAffine(ClientContext &context, const Expression &expr, idx_t decide_index,
                                    double &scale, double &offset) {
	scale = 1.0;
	offset = 0.0;
	const Expression *current = UnwrapDecideCasts(expr, decide_index);
	if (current->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return current;
	}
	auto &func = current->Cast<BoundFunctionExpression>();
	const auto &name = func.function.name;
	auto constant = [&](const Expression &candidate, double &value) {
		return TryEvaluateFoldableDoubleNoThrow(context, candidate, value) && std::isfinite(value);
	};
	double c;
	if (name == "-" && func.children.size() == 1) {
		auto *term = PeelAffine(context, *func.children[0], decide_index, scale, offset);
		scale = -scale;
		offset = -offset;
		return term;
	}
	if (func.children.size() != 2) {
		return current;
	}
	if (name == "*") {
		for (idx_t side = 0; side < 2; side++) {
			if (constant(*func.children[side], c) && c != 0.0) {
				auto *term = PeelAffine(context, *func.children[1 - side], decide_index, scale, offset);
				scale *= c;
				offset *= c;
				return term;
			}
		}
		return current;
	}
	if (name == "/" && constant(*func.children[1], c) && c != 0.0) {
		auto *term = PeelAffine(context, *func.children[0], decide_index, scale, offset);
		scale /= c;
		offset /= c;
		return term;
	}
	if (name == "+") {
		for (idx_t side = 0; side < 2; side++) {
			if (constant(*func.children[side], c)) {
				auto *term = PeelAffine(context, *func.children[1 - side], decide_index, scale, offset);
				offset += c;
				return term;
			}
		}
		return current;
	}
	if (name == "-") {
		if (constant(*func.children[1], c)) {
			auto *term = PeelAffine(context, *func.children[0], decide_index, scale, offset);
			offset -= c;
			return term;
		}
		if (constant(*func.children[0], c)) {
			auto *term = PeelAffine(context, *func.children[1], decide_index, scale, offset);
			scale = -scale;
			offset = c - offset;
			return term;
		}
	}
	return current;
}

//! Resolve `expr`, one bare decision reference under casts, to its variable. Anything
//! else -- a multi-variable LHS (`x - 3*z_1 - 5*z_2 = 0`, the IN linking row), a product
//! -- is a genuine relation, not a bound.
static bool TryMatchBareDecision(const Expression &expr, LogicalDecide &decide, AbsorptionTarget &out) {
	auto *term = UnwrapDecideCasts(expr, decide.decide_index);
	if (term->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &colref = term->Cast<BoundColumnRefExpression>();
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
		out.scale = 1.0;
		out.offset = 0.0;
		return true;
	}
	return false;
}

//! Resolve `expr`, one decision in an affine spelling (`2 * x`, `-x`, `x / 2`,
//! `2 * (x - 1)`), to its variable and the spelling it stands in.
static bool TryMatchAbsorptionTarget(ClientContext &context, const Expression &expr, LogicalDecide &decide,
                                     AbsorptionTarget &out) {
	double scale, offset;
	auto *term = PeelAffine(context, expr, decide.decide_index, scale, offset);
	if (!TryMatchBareDecision(*term, decide, out)) {
		return false;
	}
	out.scale = scale;
	out.offset = offset;
	return true;
}

//! Resolve an easy `max(x) <= K` / `min(x) >= K` (MINMAX_EASY_REWRITE_TAG), in an affine
//! spelling outside the reducer (`2 * min(x)`, `-min(x)`, `2 * (min(x) - 1)`), to its
//! decision. The member is a bare decision: `max(2 * x) <= K` bounds the member `2 * x`,
//! and stays one row per member. It bounds every row its reducer reads: with no reducer
//! filter and no front `when` (whose wrapper this pass never enters) that is every kept
//! row, so it is the column bound `x <= K` / `x >= K`, and a negative K lets x go negative
//! as `x >= -5` does (D41). The reducer stays in the tree, so EXPLAIN and DIAGNOSE read
//! the clause as written.
static bool TryMatchEasyExtremumTarget(ClientContext &context, const BoundComparisonExpression &comp,
                                       LogicalDecide &decide, AbsorptionTarget &out) {
	if (!HasDecideTag(comp.GetAlias(), MINMAX_EASY_REWRITE_TAG)) {
		return false;
	}
	double scale, offset;
	auto *term = PeelAffine(context, *comp.left, decide.decide_index, scale, offset);
	if (term->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	auto &agg = term->Cast<BoundAggregateExpression>();
	if ((agg.function.name != "min" && agg.function.name != "max") || agg.filter || agg.children.size() != 1 ||
	    !TryMatchBareDecision(*agg.children[0], decide, out)) {
		return false;
	}
	out.scale = scale;
	out.offset = offset;
	return true;
}

void DecideOptimizer::AbsorbBoundsInExpression(Expression &expr, LogicalDecide &decide) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		// `when c:` -- a conditional per-row constraint must NOT contribute to a global
		// bound: `when c: x <= 0` does not mean `x <= 0` everywhere. This is why the pass keeps
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
		// A comparison holds only at the rows where every frame it reads outside its
		// reducers selects a position (syntax_reference.md §5.6) -- a condition, as a `when`
		// is -- so it must not become a global bound either. The row an ABS lowers from
		// `abs(x - at(previous: x) over (...)) <= K` carries its clause's id, not the frame.
		for (auto &frame : decide.frames) {
			if (frame.SkipsInstancesOf(source_clause_id)) {
				return;
			}
		}
		AbsorptionTarget target;
		if (!TryMatchAbsorptionTarget(optimizer.context, *comp.left, decide, target) &&
		    !TryMatchEasyExtremumTarget(optimizer.context, comp, decide, target)) {
			break;
		}

		// The bound is whatever the other side folds to before the query runs: a literal
		// under casts, or the `1 + -3` canonicalization leaves of `x - 1 >= -3`. A side that
		// does not fold -- a column, a subquery -- is data, and the comparison stays a row.
		//
		// A non-finite bound is left for the constraint path, which already reads it the
		// way the solver does: `x <= +inf` is no bound, `x >= +inf` has no solution, and
		// NaN is an error naming the arithmetic. Absorbing it instead would write it into
		// the column box, where min/max against the ±1e30 sentinels keep an upper bound
		// but not a lower one — the same infinity silently vanishing in one direction and
		// reaching the model validator as an internal error in the other.
		double k;
		if (!TryEvaluateFoldableDouble(optimizer.context, *comp.right, k) || !std::isfinite(k)) {
			break;
		}
		// The spelling the decision stands in is undone on the bound: a shift moves
		// across, a factor divides and, when negative, reverses the relation. `2 * x >= -4`
		// is `x >= -2`, and so are `-x <= 2` and `2 * (x - 1) >= -6`.
		k = (k - target.offset) / target.scale;
		const auto relation = target.scale < 0 ? FlipComparisonExpression(comp.type) : comp.type;

		// `=` intersects both sides rather than assigning, so `x = 5 AND x = 10` inverts
		// the box and the conflict is caught instead of resolving to whichever was
		// written last. A strict `<` / `>` normalizes into the bound for an integer
		// (`x < 10` is `x <= 9`), carrying the user's literal as `typed_k` so the
		// diagnosis re-quotes `< 10`. A REAL has no such normalization, so it declines
		// and the constraint path rejects it with a message naming the clause. The bound
		// steps onto the lattice as every row does (`x > 2.5` is `x >= 3`).
		const double below = std::ceil(k) - 1.0;
		const double above = std::floor(k) + 1.0;
		bool absorbed = true;
		bool recorded = false;
		switch (relation) {
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			recorded = target.Absorb('<', k, false, 0.0, source_clause_id, removal_group_id);
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			recorded = target.Absorb('>', k, false, 0.0, source_clause_id, removal_group_id);
			break;
		case ExpressionType::COMPARE_EQUAL:
			recorded = target.Absorb('=', k, false, 0.0, source_clause_id, removal_group_id);
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			absorbed = target.is_integer;
			if (absorbed) {
				recorded = target.Absorb('<', below, true, k, source_clause_id, removal_group_id);
			}
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			absorbed = target.is_integer;
			if (absorbed) {
				recorded = target.Absorb('>', above, true, k, source_clause_id, removal_group_id);
			}
			break;
		default:
			absorbed = false;
			break;
		}

		if (absorbed) {
			// The comparison stays in the tree so EXPLAIN keeps rendering what the user
			// wrote; the tag is what stops it also becoming a model row. A clause is one
			// knob per kept row or class in an expanded diagnosis (D28), which one column
			// bound cannot say, so a recorded one is flattened for the repair model too.
			AddDecideTag(comp.alias, ABSORBED_BOUND_TAG);
			if (recorded) {
				AddDecideTag(comp.alias, ABSORBED_CLAUSE_ROWS_TAG);
			}
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
	decide.user_absorbed_bounds.clear();

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
