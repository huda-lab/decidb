//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/decide/decide_prepared_model.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/decide.hpp"
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
// absorbed variable box and the composed MIN/MAX terms.

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
	//! An objective AVG's `by` key, into LogicalDecide::group_keys: it divides each row by
	//! its group's count, not the objective's (a nested `sum(per k: avg(e) by (k))`, D75).
	//! INVALID_INDEX for one group.
	idx_t avg_group_key = DConstants::INVALID_INDEX;
	LinearTermReduction reduction = LinearTermReduction::NONE;
	//! Entity scope this term's reducer is qualified by (`sum(D: ...)`), or
	//! INVALID_INDEX for an unqualified reducer. Indexes entity_scopes /
	//! entity_mappings; coefficient evaluation keeps one row per tuple identity
	//! of that relation and zeroes the duplicates the join introduced.
	idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;

	DecideTerm(idx_t var_idx, unique_ptr<Expression> coef, int s = 1)
	    : variable_index(var_idx), coefficient(std::move(coef)), sign(s) {
	}
};

//! A bilinear term in a constraint: `sign * coefficient * var_a * var_b`.
struct BilinearConstraintTerm {
	idx_t var_a;
	idx_t var_b;
	unique_ptr<Expression> coefficient; // Data coefficient (or nullptr for 1.0)
	int sign = 1;
	unique_ptr<Expression> filter; // Optional filter: the reducer's own `when`
	bool avg_scale = false;        // True when this term came from AVG and needs 1/N scaling
	//! Entity scope this term's reducer is qualified by (`sum(D: ...)`), or
	//! INVALID_INDEX for an unqualified reducer. Indexes entity_scopes /
	//! entity_mappings; coefficient evaluation keeps one row per tuple identity
	//! of that relation and zeroes the duplicates the join introduced.
	idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;
};

//! One canonical comparison, flattened into terms.
struct DecideConstraint {
	vector<DecideTerm> lhs_terms;    // All additive terms from LHS
	unique_ptr<Expression> rhs_expr; // RHS expression (may contain aggregates)
	ExpressionType comparison_type;  // COMPARE_LESSTHANOREQUALTO or GREATERTHANOREQUALTO
	bool lhs_is_aggregate = false;   // True if original LHS was an aggregate (e.g., SUM(...))
	bool was_minmax_easy = false;    // An easy-direction MIN/MAX kept its reducer (MINMAX_EASY_REWRITE_TAG): one row per member, the bound reduced over each `by` group.
	idx_t minmax_clause_idx = DConstants::INVALID_INDEX; // index into LogicalDecide::minmax_clause_labels
	string minmax_agg_type;                                 // "min" or "max" (empty if not minmax)
	idx_t ne_clause_idx = DConstants::INVALID_INDEX;     // index into LogicalDecide::ne_clause_labels
	idx_t abs_aux_idx = DConstants::INVALID_INDEX;          // ABS auxiliary this envelope row bounds
	bool abs_is_pos_bound = false;                          // true=C1 (aux >= inner), false=C2 (aux >= -inner)
	unique_ptr<Expression> when_condition;                  // the clause's `when` (nullptr = every row)
	//! A clause reads only the rows its `when` keeps (a NULL on another row is never
	//! evaluated), NULL is a key value, every bound-side reducer reads its own `by` group of
	//! the kept rows, and LogicalDecide::scope_checks have passed before it is evaluated.
	//! Scoped, aggregate shape: the `by` key its decision reducers share, which groups its
	//! rows (index into LogicalDecide::group_keys); INVALID_INDEX for `by ()`.
	idx_t group_key = DConstants::INVALID_INDEX;
	//! What a scoped clause's `per K:` / `per ()` does. ONE_ROW_PER_CLASS: the per-row
	//! shape emits one row per class of `class_key` among the kept rows, the class's
	//! first (`per ()` is one class). NAMES_CLASSES: the aggregate shape and the scoped
	//! easy MIN/MAX emit their own rows, and the key only names the classes each `by`
	//! group's row stands for, which a diagnosis reports one by one. NONE: no `per`
	//! written, so every kept row of a group is a class of its own.
	enum class PerKeyRole : uint8_t { NONE, ONE_ROW_PER_CLASS, NAMES_CLASSES };
	PerKeyRole per_key = PerKeyRole::NONE;
	vector<unique_ptr<Expression>> class_key;
	//! ONE_ROW_PER_CLASS: the clause's variant signature (LogicalDecide::signatures), which
	//! splits each class into one row per distinct value of its fields (D64); INVALID_INDEX
	//! when it has none, so one row per class.
	idx_t signature = DConstants::INVALID_INDEX;
	ConstraintKind kind = ConstraintKind::USER_PARAMETER;
	//! Stable origin in LogicalDecide::constraint_sources.
	idx_t source_clause_id = DConstants::INVALID_INDEX;
	//! Atomic DROP identity, independent of display and elastic grouping provenance.
	idx_t removal_group_id = DConstants::INVALID_INDEX;

	// Bilinear terms in constraint (non-Boolean pairs left by optimizer)
	vector<BilinearConstraintTerm> bilinear_terms;
	bool has_bilinear = false;

	// Quadratic groups in constraint: each POWER(expr, 2) or (expr)*(expr) self-product
	// becomes a separate group. The model builder computes an outer-product Q for each
	// group independently, then accumulates all into the same QuadraticConstraint.
	// This is necessary because POWER(x-t,2) + POWER(y-s,2) != POWER(x-t+y-s, 2).
	struct QuadraticGroup {
		vector<DecideTerm> inner_terms; // Inner linear expression of POWER(inner, 2)
		double sign = 1.0;              // +1, -1, or scalar (from negation/scaling)
		//! Query-wide reducer scale not known until the relational input runs.
		unique_ptr<Expression> scale;
		bool scale_divides = false;
		unique_ptr<Expression> filter; // Optional filter: the reducer's own `when`
		bool avg_scale = false;        // True when this group came from AVG and needs 1/N scaling
		//! Entity scope this term's reducer is qualified by (`sum(D: ...)`), or
		//! INVALID_INDEX for an unqualified reducer. Indexes entity_scopes /
		//! entity_mappings; coefficient evaluation keeps one row per tuple identity
		//! of that relation and zeroes the duplicates the join introduced.
		idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;

		QuadraticGroup() = default;
	};
	vector<QuadraticGroup> quadratic_groups;
	bool has_quadratic = false;

	DecideConstraint() = default;
};

//! The objective, flattened into terms. Supports both linear objectives (terms
//! only) and quadratic objectives of the form MINIMIZE SUM((linear_expr)^2) +
//! linear_terms.
struct DecideObjective {
	vector<DecideTerm> terms;              // Linear objective terms
	unique_ptr<Expression> when_condition; // the objective's `when` (nullptr = every row)
	//! The objective reads only the rows its `when` keeps, and a term beside its reducers
	//! counts once, read on the first kept row.

	//! Quadratic objective: the inner linear expression of each SUM(POWER(expr, 2)) term.
	//! When non-empty, the objective includes a quadratic component: sign * SUM((inner_expr)^2).
	//! sign = +1.0 for SUM(POWER(expr, 2)), sign = -1.0 for SUM(-POWER(expr, 2)).
	vector<DecideTerm> squared_terms;
	bool has_quadratic = false;
	double quadratic_sign = 1.0;

	//! Bilinear objective terms: x_a * x_b with data coefficient.
	//! These are products of two different DECIDE variables where neither is Boolean
	//! (Boolean cases are linearized by the optimizer into McCormick auxiliary variables).
	struct BilinearTerm {
		idx_t var_a;                        // First DECIDE variable index
		idx_t var_b;                        // Second DECIDE variable index
		unique_ptr<Expression> coefficient; // Data coefficient expression (or nullptr for 1.0)
		int sign = 1;                       // +1 or -1
		unique_ptr<Expression> filter;      // Optional filter: the reducer's own `when`
		bool avg_scale = false;             // True when this term came from AVG and needs 1/N scaling
		idx_t avg_group_key = DConstants::INVALID_INDEX; //!< as DecideTerm's
		//! Entity scope this term's reducer is qualified by (`sum(D: ...)`), or
		//! INVALID_INDEX for an unqualified reducer. Indexes entity_scopes /
		//! entity_mappings; coefficient evaluation keeps one row per tuple identity
		//! of that relation and zeroes the duplicates the join introduced.
		idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;
		//! SUM inside a reducer, NONE outside every reducer (counted once), as for DecideTerm.
		LinearTermReduction reduction = LinearTermReduction::NONE;
	};
	vector<BilinearTerm> bilinear_terms;
	bool has_bilinear = false;

	DecideObjective() = default;
};

//! Everything DecideLinearFormBuilder (stage 05) produces, carried on
//! LogicalDecide and moved into PhysicalDecide at physical planning.
//!
//! `objective` is null when the query has no objective, or when the objective
//! is neither an aggregate nor a query-wide decision -- the same two cases in
//! which physical analysis used to leave its `objective` pointer unset.
struct DecidePreparedModel {
	vector<unique_ptr<DecideConstraint>> constraints;
	//! Scoped bounds the optimizer folded into a column box (ABSORBED_BOUND_TAG). The box
	//! enforces them, so the solved model has no row for them; a repair model emits their
	//! rows, one per kept row or per class of a `per` key, so an expanded diagnosis loosens
	//! one the way its clause scopes it (D28). Evaluated under DIAGNOSE only.
	vector<unique_ptr<DecideConstraint>> absorbed_constraints;
	unique_ptr<DecideObjective> objective;
};

} // namespace duckdb
