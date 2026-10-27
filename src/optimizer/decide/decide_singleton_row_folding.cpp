//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/optimizer/decide/decide_singleton_row_folding.cpp
//
// DECIDE singleton-row folding: fold a literal bound into a column box. See decide_optimizer.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
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
// Singleton-row folding: a bound, not a row
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

//! One decision variable, resolved, plus the type facts folding needs about it, and
//! the affine spelling it stands in: the comparison's side is `scale * x + offset`
//! (`2 * x`: 2 and 0; `-x`: -1 and 0; `2 * (x - 1)`: 2 and -2).
struct FoldingTarget {
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
	bool Fold(char sense, double k, bool strict, double typed_k, idx_t source_clause_id, idx_t removal_group_id) const {
		DecideFoldSingletonRow(decide->singleton_row_boxes[var_idx], sense, k);
		if (!ShouldRecord(sense, k)) {
			return false;
		}
		decide->user_singleton_row_bounds.push_back(
		    {var_idx, sense, k, strict, typed_k, source_clause_id, removal_group_id});
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
static const Expression *PeelAffine(ClientContext &context, const Expression &expr, idx_t decide_index, double &scale,
                                    double &offset) {
	scale = 1.0;
	offset = 0.0;
	const Expression *current = UnwrapDecideCasts(expr, decide_index);
	if (current->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return current;
	}
	auto &func = current->Cast<BoundFunctionExpression>();
	const auto &name = func.function.name;
	auto constant = [&](const Expression &candidate, double &value) {
		return TryEvaluateFoldableDouble(context, candidate, value, FOLD_CATCH_ALL) && std::isfinite(value);
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
static bool TryMatchBareDecision(const Expression &expr, LogicalDecide &decide, FoldingTarget &out) {
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
static bool TryMatchFoldingTarget(ClientContext &context, const Expression &expr, LogicalDecide &decide,
                                  FoldingTarget &out) {
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
                                       LogicalDecide &decide, FoldingTarget &out) {
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

//! The solver's reading of a comparison that belongs in a variable's box: the decision, the side it tightens and the
//! value it tightens to. This is the one place that says which comparisons are folded, shared with direct solve
//! through DecideIsSingletonRowBound so the two cannot disagree about whether a bound replaces a variable's default.
struct SingletonRowBound {
	FoldingTarget target;
	char sense = '<';
	double k = 0.0;
	bool strict = false;
	//! The user's bound, moved onto the bare decision, when `strict` normalized it by a whole unit.
	double typed_k = 0.0;
	//! True when the clause is spelled as one bare decision against a literal under casts (`x <= 10`, `x > 2.5`).
	bool plain = false;
};

static bool TryReadSingletonRowBound(ClientContext &context, LogicalDecide &decide,
                                     const BoundComparisonExpression &comp, SingletonRowBound &out) {
	bool extremum = false;
	if (!TryMatchFoldingTarget(context, *comp.left, decide, out.target)) {
		if (!TryMatchEasyExtremumTarget(context, comp, decide, out.target)) {
			return false;
		}
		extremum = true;
	}

	// A non-finite bound is left for the constraint path, which already reads it the
	// way the solver does: `x <= +inf` is no bound, `x >= +inf` has no solution, and
	// NaN is an error naming the arithmetic. Folding it instead would write it into
	// the column box, where min/max against the ±1e30 sentinels keep an upper bound
	// but not a lower one — the same infinity silently vanishing in one direction and
	// reaching the model validator as an internal error in the other.
	//
	// The bound is whatever the other side folds to before the query runs: a literal
	// under casts, or the `1 + -3` canonicalization leaves of `x - 1 >= -3`. A side that
	// does not fold -- a column, a subquery -- is data, and the comparison stays a row.
	double k;
	if (!TryEvaluateFoldableDouble(context, *comp.right, k, FOLD_PLAIN) || !std::isfinite(k)) {
		return false;
	}
	// StripCastsForIdentity reads only the innermost node's class, never its value.
	out.plain = !extremum && out.target.scale == 1.0 && out.target.offset == 0.0 &&
	            StripCastsForIdentity(*comp.right)->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT;
	// The spelling the decision stands in is undone on the bound: a shift moves
	// across, a factor divides and, when negative, reverses the relation. `2 * x >= -4`
	// is `x >= -2`, and so are `-x <= 2` and `2 * (x - 1) >= -6`.
	k = (k - out.target.offset) / out.target.scale;
	const auto relation = out.target.scale < 0 ? FlipComparisonExpression(comp.type) : comp.type;
	out.k = k;

	// `=` intersects both sides rather than assigning, so `x = 5 AND x = 10` inverts
	// the box and the conflict is caught instead of resolving to whichever was
	// written last. A strict `<` / `>` normalizes into the bound for an integer
	// (`x < 10` is `x <= 9`, `x > 0.5` is `x >= 1`), carrying the user's literal as `typed_k` so the
	// diagnosis re-quotes `< 10`. A REAL has no such normalization, so it declines
	// and the constraint path rejects it with a message naming the clause. The bound
	// steps onto the lattice as every row does (`x > 2.5` is `x >= 3`).
	switch (relation) {
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		out.sense = '<';
		return true;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		out.sense = '>';
		return true;
	case ExpressionType::COMPARE_EQUAL:
		out.sense = '=';
		return true;
	case ExpressionType::COMPARE_LESSTHAN:
		out.sense = '<';
		out.k = DecideStrictBound(false, k);
		out.strict = true;
		out.typed_k = k;
		return out.target.is_integer;
	case ExpressionType::COMPARE_GREATERTHAN:
		out.sense = '>';
		out.k = DecideStrictBound(true, k);
		out.strict = true;
		out.typed_k = k;
		return out.target.is_integer;
	default:
		return false;
	}
}

bool DecideIsSingletonRowBound(ClientContext &context, LogicalDecide &decide,
                               const BoundComparisonExpression &comparison, bool &plain) {
	SingletonRowBound bound;
	if (!TryReadSingletonRowBound(context, decide, comparison, bound)) {
		plain = false;
		return false;
	}
	plain = bound.plain;
	return true;
}

bool DecideBoxContradictsDomain(const DecideBoundBox &box, bool is_bool) {
	double floor = box.EffectiveLower();
	// A BOOLEAN's floor is 0 whatever lower bound was written (a lower bound below 0 changes nothing), so an upper
	// bound below 0 contradicts it even when the written lower bound is negative (`x = -1`).
	return (box.upper < 0.0 && (floor >= 0.0 || is_bool)) || (is_bool && floor > 1.0);
}

void DecideOptimizer::FoldSingletonRowsInExpression(Expression &expr, LogicalDecide &decide) {
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
			FoldSingletonRowsInExpression(*child, decide);
		}
		break;
	}

	case ExpressionClass::BOUND_COMPARISON: {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		idx_t source_clause_id = DConstants::INVALID_INDEX;
		TryParseSourceClauseTag(comp.GetAlias(), source_clause_id);
		idx_t removal_group_id = DConstants::INVALID_INDEX;
		TryParseRemovalGroupTag(comp.GetAlias(), removal_group_id);

		SingletonRowBound bound;
		if (!TryReadSingletonRowBound(optimizer.context, decide, comp, bound)) {
			break;
		}
		// The comparison stays in the tree so EXPLAIN keeps rendering what the user
		// wrote; the tag is what stops it also becoming a model row. A clause is one
		// knob per kept row or class in an expanded diagnosis (D28), which one column
		// bound cannot say, so a recorded one is flattened for the repair model too.
		AddDecideTag(comp.alias, SINGLETON_ROW_BOUND_TAG);
		if (bound.target.Fold(bound.sense, bound.k, bound.strict, bound.typed_k, source_clause_id, removal_group_id)) {
			AddDecideTag(comp.alias, SINGLETON_ROW_CLAUSE_ROWS_TAG);
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

void DecideOptimizer::FoldSingletonRows(LogicalDecide &decide) {
	// Sized here rather than at construction so every auxiliary variable created by the
	// preceding passes is already counted.
	idx_t num_decide_vars = decide.decide_variables.size();
	decide.singleton_row_boxes.assign(num_decide_vars, DecideBoundBox());
	decide.user_singleton_row_bounds.clear();

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
	// before. Folding only ever narrows (`std::min`), and a user restatement like
	// `x <= 1` is already treated as a harmless no-op against the intrinsic box, so
	// seeding it changes nothing about how a written bound is recorded.
	for (idx_t i = 0; i < num_decide_vars; i++) {
		if (i < decide.is_boolean_var.size() && decide.is_boolean_var[i]) {
			decide.singleton_row_boxes[i].upper = 1.0;
			// The floor is just as rigid. A written lower bound only ever raises it (`max`), so `x >= -3`
			// leaves the box at [0,1] and a bound that cannot hold (`x >= 2`, `x <= -1`) inverts it, which
			// the solve reports as infeasible. Without this the box would widen to [-3,1], and x would
			// take a value that is not a Boolean.
			DecideFoldSingletonRow(decide.singleton_row_boxes[i], '>', 0.0);
		}
	}

	if (decide.decide_constraints) {
		FoldSingletonRowsInExpression(*decide.decide_constraints, decide);
	}
}

} // namespace duckdb
