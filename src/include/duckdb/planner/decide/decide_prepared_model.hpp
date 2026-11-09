//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/planner/decide/decide_prepared_model.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// The prepared linear form
//===--------------------------------------------------------------------===//
//
// The contract between DECIDE optimization (stage 05) and physical execution
// (stage 08). Stage 05 flattens each canonical constraint and the objective into
// additive terms; stage 08 evaluates each term's coefficient against the
// relational input and never re-derives the shape.
//
// A coefficient stays an unevaluated `Expression` because it may reference data
// columns -- `SUM(x * price)` yields the coefficient `price`, which is a number
// only once a row exists. Everything ELSE about a term (which variable it names,
// its sign, which reducer produced it, which filter applies) is a fact about
// types and structure, and is decided here.
//
// These structures live at stage 03 rather than 05 because `LogicalDecide` is
// what carries them across the plan boundary, the same way it carries the
// folded variable box and the composed MIN/MAX terms.

//! A single additive term: `sign * coefficient * variable`.
//! `variable_index` is DConstants::INVALID_INDEX for a constant (variable-free)
//! term. Used for linear expressions and for the inner expression of a
//! quadratic group.
struct DecideTerm {
	idx_t variable_index;               // Which DECIDE variable (or INVALID_INDEX for constants)
	unique_ptr<Expression> coefficient; // Row-varying expression to evaluate later
	int sign = 1;                       // +1 or -1, applied at coefficient evaluation time
	unique_ptr<Expression> filter;      // Optional filter: the reducer's own `when`
	bool avg_scale = false;             // True when this term came from AVG and needs 1/N scaling
	LinearTermReduction reduction = LinearTermReduction::NONE;
	//! Keyed scope this term's reducer is qualified by (`sum(D: ...)`), or
	//! INVALID_INDEX for an unqualified reducer. Indexes keyed_scopes /
	//! keyed_mappings; coefficient evaluation keeps one row per tuple identity
	//! of that relation and zeroes the duplicates the join introduced.
	idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;
	//! The values the user wrote that `coefficient` is computed from when DeciDB computed it (like terms merged, a
	//! reducer's factor applied), in reading order; empty when `coefficient` is as written. A coefficient that is not
	//! finite names the first of them a column holds that is not finite, and its clause when there is none: DeciDB's
	//! own arithmetic left the DOUBLE range (DecideInvalidValueMessage). Only a row where it fails reads them.
	vector<unique_ptr<Expression>> inputs;

	DecideTerm(idx_t var_idx, unique_ptr<Expression> coef, int s = 1)
	    : variable_index(var_idx), coefficient(std::move(coef)), sign(s) {
	}
};

//! A term of degree 2: `sign * weight * l * r` on each row its reducer reads, where `l` and `r` are linear forms in
//! the row's decisions. The model builder adds it to Q as the rank-one update `weight * l * rᵀ`, symmetrized, and its
//! cross terms with a form's constant to the linear part (ilp_model_builder.cpp, AddProductGroup).
//!
//! A square `w * POWER(f, 2)` is `l = f, r = f`, flagged `square` with `right` empty: Q gets `w * f * fᵀ`, which is
//! positive semi-definite when `w` is positive. `w * x * y` is `l = x, r = y`. One struct carries both, in a
//! constraint and in the objective, so every stage reads one shape of degree 2 (CAN-1).
struct DecideProductGroup {
	//! The coefficient the two forms are multiplied by, never a decision; null for 1. A reducer's factor
	//! (`2 * SUM(POWER(x, 2))`) is multiplied in like a linear term's (ApplyScaleToExtracted).
	unique_ptr<Expression> weight;
	int sign = 1;
	vector<DecideTerm> left;
	//! The second form; empty for a square, which multiplies `left` by itself.
	vector<DecideTerm> right;
	bool square = false;
	//! The sign of `sign * weight` when the plan knows it (ScaleSignAtPlanTime): +1 or -1, and 0 for a weight known
	//! only when the query runs. Whether the group is convex is read from it (IsNonconvexProductGroup).
	int weight_sign = 1;
	unique_ptr<Expression> filter; // the reducer's own `when`
	bool avg_scale = false;        // the group came from AVG and needs 1/N scaling
	//! Keyed scope the group's reducer is qualified by (`sum(D: ...)`), or INVALID_INDEX, as for DecideTerm.
	idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;
	//! SUM inside a reducer, NONE outside every reducer (counted once), as for DecideTerm. Read by the objective only.
	LinearTermReduction reduction = LinearTermReduction::NONE;
	//! The values `weight` is computed from (DecideTerm::inputs).
	vector<unique_ptr<Expression>> inputs;
};

//! The scope of a clause that has left the constraint tree, owned: the rows its `when` keeps and what its `per K:`
//! does. Built from the borrowed DecideEnclosingScope (decide_constraint_walk.hpp) the clause sat under, and carried
//! by every holder of such a clause (DecideConstraint, LogicalDecide::ComposedMinMaxConstraint) as this one parameter
//! object. A clause reads only the rows its `when` keeps (a NULL on another row is never evaluated), and
//! LogicalDecide::scope_checks have passed before it is evaluated.
struct DecideClauseScope {
	//! The clause's `when`, or null for every row.
	unique_ptr<Expression> when;
	//! What the clause's `per K:` / `per ()` does. ONE_ROW_PER_CLASS: the per-row shape emits one row per class of
	//! `class_key` among the kept rows, the class's first (`per ()` is one class). NAMES_CLASSES: the aggregate shape,
	//! the scoped easy MIN/MAX and the composed MIN/MAX emit their own rows, and the key only names the classes each
	//! row stands for, which a diagnosis reports one by one (D28). NONE: no `per` written, so every kept row of a
	//! group is a class of its own.
	enum class PerRole : uint8_t { NONE, ONE_ROW_PER_CLASS, NAMES_CLASSES };
	PerRole per = PerRole::NONE;
	//! The `per` key (index into LogicalDecide::keys); INVALID_INDEX with NONE and for `per ()`.
	idx_t class_key = DConstants::INVALID_INDEX;

	DecideClauseScope() = default;
	//! The scope of a clause under `enclosing`: its `when`, and its `per` naming the classes its rows stand for.
	explicit DecideClauseScope(const DecideEnclosingScope &enclosing) {
		if (enclosing.Condition()) {
			when = enclosing.Condition()->Copy();
		}
		if (enclosing.per_wrapper) {
			per = PerRole::NAMES_CLASSES;
			class_key = enclosing.PerKey();
		}
	}
};

//! One canonical comparison, flattened into terms.
struct DecideConstraint {
	vector<DecideTerm> lhs_terms;    // All additive terms from LHS
	unique_ptr<Expression> rhs_expr; // RHS expression (may contain aggregates)
	ExpressionType comparison_type;  // COMPARE_LESSTHANOREQUALTO or GREATERTHANOREQUALTO
	bool lhs_is_aggregate = false;   // True if original LHS was an aggregate (e.g., SUM(...))
	bool was_minmax_easy = false; // An easy-direction MIN/MAX kept its reducer (MINMAX_EASY_REWRITE_TAG): one row per
	                              // member, the bound reduced over each `by` group.
	idx_t minmax_clause_idx = DConstants::INVALID_INDEX; // index into LogicalDecide::minmax_clause_labels
	string minmax_agg_type;                              // "min" or "max" (empty if not minmax)
	idx_t ne_clause_idx = DConstants::INVALID_INDEX;     // index into LogicalDecide::ne_clause_sources
	idx_t abs_aux_idx = DConstants::INVALID_INDEX;       // ABS auxiliary this envelope row bounds
	bool abs_is_pos_bound = false;                       // true=C1 (aux >= inner), false=C2 (aux >= -inner)
	//! The clause's `when` and `per`.
	DecideClauseScope scope;
	//! Aggregate shape: the `by` key its decision reducers share, which groups its rows (index into
	//! LogicalDecide::keys); INVALID_INDEX for `by ()`. Read off the comparison, not its scope. Every bound-side
	//! reducer reads its own `by` group of the kept rows.
	idx_t group_key = DConstants::INVALID_INDEX;
	ConstraintKind kind = ConstraintKind::USER_PARAMETER;
	//! Stable origin in LogicalDecide::constraint_sources.
	idx_t source_clause_id = DConstants::INVALID_INDEX;
	//! Atomic DROP identity, independent of display and elastic grouping provenance.
	idx_t removal_group_id = DConstants::INVALID_INDEX;
	//! A per-row bound on one decision: the values of the decision it admits on each row (DecideSingletonRow), as
	//! DOUBLE expressions over the row and the inputs `admitted_a` and `admitted_r`, ±infinity on an open side. Null
	//! for any other constraint. Stage 08 evaluates the inputs once on the rows `admitted_member` keeps (every kept
	//! row without one) and the ends over them; a row that is no member admits every value. The model builder folds
	//! them into the decision's column instead of adding the row; a data bound narrows the column and never replaces
	//! its floor of 0 (D41).
	unique_ptr<Expression> admitted_lower;
	unique_ptr<Expression> admitted_upper;
	unique_ptr<Expression> admitted_a;
	unique_ptr<Expression> admitted_r;
	unique_ptr<Expression> admitted_member;
	//! The decision whose default floor of 0 this bound replaces on the rows the clause keeps, or INVALID_INDEX: a
	//! constant lower bound under a `when` or a reducer filter (DecideSingletonRow::LiftsDefaultFloor), which
	//! singleton-row folding does not enter. An unconditional one is folded into the decision's box and sets no field
	//! here. `lifted_floor_k` is the bound as written, for the domain check.
	idx_t lifted_floor = DConstants::INVALID_INDEX;
	double lifted_floor_k = 0.0;

	//! The terms of degree 2, one group per product the left side holds: a constraint with any is a quadratic row.
	vector<DecideProductGroup> product_groups;

	DecideConstraint() = default;
};

//! The objective, flattened into terms: linear terms and product groups.
struct DecideObjective {
	vector<DecideTerm> terms;              // Linear objective terms
	unique_ptr<Expression> when_condition; // the objective's `when` (nullptr = every row)
	//! The two-level form's key (index into LogicalDecide::keys); INVALID_INDEX for none.
	idx_t two_level_key = DConstants::INVALID_INDEX;
	//! The objective reads only the rows its `when` keeps, its two-level form's key puts
	//! a row whose written key is NULL in no class, and a term beside its reducers counts once, read on the first
	//! kept row.

	//! The objective's terms of degree 2 (DecideProductGroup), summed into one Q.
	vector<DecideProductGroup> product_groups;

	DecideObjective() = default;
};

//! Everything DecideLinearFormBuilder (stage 05) produces, carried on
//! LogicalDecide and moved into PhysicalDecide at physical planning.
//!
//! `objective` is null when the query has no objective, or when the objective
//! is neither an aggregate nor a query-wide decision.
struct DecidePreparedModel {
	vector<unique_ptr<DecideConstraint>> constraints;
	//! Scoped bounds the optimizer folded into a column box (SINGLETON_ROW_BOUND_TAG). The box
	//! enforces them, so the solved model has no row for them; a repair model emits their
	//! rows, one per kept row or per class of a `per` key, so an expanded diagnosis loosens
	//! one the way its clause scopes it (D28). Evaluated under DIAGNOSE only.
	vector<unique_ptr<DecideConstraint>> singleton_row_constraints;
	unique_ptr<DecideObjective> objective;
};

} // namespace duckdb
