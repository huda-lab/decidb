//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/planner/decide/decide_tree_terms.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// The tree's term records
//===--------------------------------------------------------------------===//
//
// What stage 05's lowering (LowerDecideTree) makes of each clause, the objective and a helper's
// reducer, held in the node that holds it (DecideBody): additive terms, which stage 08 evaluates against the
// relational input without re-deriving their shape.
//
// A coefficient stays an unevaluated `Expression` because it may reference data
// columns -- `SUM(x * price)` yields the coefficient `price`, which is a number
// only once a row exists. Everything ELSE about a term (which variable it names,
// its sign, which reducer produced it, which filter applies) is a fact about
// types and structure, and is decided here.

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
	//! An objective AVG's `by` key, into LogicalDecide::keys: it divides each row by
	//! its group's count, not the objective's (a nested `sum(per k: avg(e) by (k))`, D81).
	//! INVALID_INDEX for one group.
	idx_t avg_group_key = DConstants::INVALID_INDEX;
	LinearTermReduction reduction = LinearTermReduction::NONE;
	//! Keyed scope this term's reducer is qualified by (`sum(per D: ...)`), or
	//! INVALID_INDEX for an unqualified reducer. Indexes keyed_scopes /
	//! keyed_mappings; coefficient evaluation keeps one row per tuple identity
	//! of that relation and zeroes the duplicates the join introduced.
	idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;
	//! The exact G whose `per` qualifies this term. Scope indexes may be reused by several reducer occurrences.
	idx_t qualifier_reducer = DConstants::INVALID_INDEX;
	//! A frame selector's payload term (language/guide.md §5.6): the variable is read at
	//! the row frame `frame_read` (into LogicalDecide::frames) selects for each row, not at
	//! the row itself. Its coefficient reads a READ frame column (DecideFrameColumn).
	//! INVALID_INDEX for every other term.
	idx_t frame_read = DConstants::INVALID_INDEX;
	//! The tree's G whose operand entries this term ranges over (DecideTreeConstruction::ReducerChild): written by
	//! stage 05 on the terms a reducer produced, INVALID for a term outside every reducer and whenever no tree is
	//! built. Stage 05 does not read it.
	idx_t child = DConstants::INVALID_INDEX;

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
	//! Keyed scope the group's reducer is qualified by (`sum(per D: ...)`), or INVALID_INDEX, as for DecideTerm.
	idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;
	//! As DecideTerm::qualifier_reducer.
	idx_t qualifier_reducer = DConstants::INVALID_INDEX;
	//! As DecideTerm::avg_group_key, for the group's AVG.
	idx_t avg_group_key = DConstants::INVALID_INDEX;
	//! SUM inside a reducer, NONE outside every reducer (counted once), as for DecideTerm. Read by the objective only.
	LinearTermReduction reduction = LinearTermReduction::NONE;
	//! As DecideTerm::child.
	idx_t child = DConstants::INVALID_INDEX;
};

//! What a clause's `per K:` / `per ()` does (DecideClauseScope::per). ONE_ROW_PER_CLASS: the per-row shape emits one
//! row per class of `class_key` and variant of `signature` among the kept rows, the variant's first (`per ()` is one
//! class). NAMES_CLASSES: the aggregate shape and the scoped easy MIN/MAX emit their own rows, and the key only names
//! the classes each row stands for, which a diagnosis reports one by one (D28). NONE: no `per` written, so every kept
//! row of a group is a class of its own. A bound clause holds NONE or NAMES_CLASSES: the tree decides
//! ONE_ROW_PER_CLASS from the clause's shape (ReadDecideClauseFacts).
enum class DecidePerRole : uint8_t { NONE, ONE_ROW_PER_CLASS, NAMES_CLASSES };

//! The scope of a clause: the rows its `when` keeps and what its `per K:` does. Carried by every holder of a clause
//! (BoundDecideClause, DecideConstraint) as this one parameter object. A clause reads only the rows its `when` keeps
//! (a NULL on another row is never evaluated), and LogicalDecide::value_checks have passed before it is evaluated.
struct DecideClauseScope {
	//! The clause's `when`, or null for every row.
	unique_ptr<Expression> when;
	//! What the clause's `per K:` / `per ()` does.
	using PerRole = DecidePerRole;
	PerRole per = PerRole::NONE;
	//! The `per` key (index into LogicalDecide::keys); INVALID_INDEX with NONE and for `per ()`.
	idx_t class_key = DConstants::INVALID_INDEX;
	//! The clause's variant signature (LogicalDecide::signatures), which splits each class into one instance per
	//! distinct value of its fields (D70); INVALID_INDEX when it has none, so one instance per class.
	idx_t signature = DConstants::INVALID_INDEX;

	//! Whether a `per` is written (`per K:` or `per ()`).
	bool HasPer() const {
		return per != PerRole::NONE;
	}
	DecideClauseScope Copy() const {
		DecideClauseScope result;
		result.when = when ? when->Copy() : nullptr;
		result.per = per;
		result.class_key = class_key;
		result.signature = signature;
		return result;
	}
	//! The scope `per K:` (`per ()` for INVALID_INDEX) with no `when`.
	static DecideClauseScope Per(idx_t class_key) {
		DecideClauseScope result;
		result.per = PerRole::NAMES_CLASSES;
		result.class_key = class_key;
		return result;
	}
};

//! The rows two conditions both keep, a copy of each as a conjunction so every conjunct stays what it is (a reducer's
//! NULL test, IsDecideNullTest); a null condition keeps every row, so the other stands alone, and null for both.
inline unique_ptr<Expression> DecideRowsKeptByBoth(const Expression *first, const Expression *second) {
	if (!first || !second) {
		const Expression *only = first ? first : second;
		return only ? only->Copy() : nullptr;
	}
	return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, first->Copy(), second->Copy());
}

//! The row shape of a canonical comparison. VALUE: its decision side mixes a decision reducer with something the
//! aggregate shape cannot hold -- a per-row or keyed decision, a factor that varies by row, a second `by` key, a
//! reducer inside another expression -- so the value rewrite (stage 05) replaces each decision reducer by a helper
//! decision defined once per `by` group. AGGREGATE: one row per group of the `by` key its decision reducers share.
//! PER_ROW: no decision reducer. INVALID: no comparison (a placeholder, an IN), which has no shape.
enum class CanonicalConstraintClass : uint8_t { PER_ROW, AGGREGATE, VALUE, INVALID };

//! Which comparison of the two `x BETWEEN lo AND hi` binds to a clause is, `>=` being the lower one; NONE for any
//! other clause. InitializeConstraintSourceInfo reads it to link the halves into the one clause the user wrote, and a
//! render does not quote a half by the BETWEEN's span.
enum class DecideBetweenHalf : uint8_t { NONE, LOWER, UPPER };

//! One row of SUCH THAT (a written comparison, a BETWEEN half, an IN, a row a rewrite adds or the TRUE placeholder it
//! leaves where it lifted a clause), or the objective. The bound clauses are a list of these (BoundDecide::clauses):
//! each holds its own scope, so no pass carries one down a tree.
struct BoundDecideClause {
	//! The comparison, the IN or the placeholder; for the objective, the minimized expression. Null for an objective
	//! the query did not write.
	unique_ptr<Expression> body;
	//! The clause's `when` and `per`.
	DecideClauseScope scope;
	//! Where the `per` stands in the query, for one the user wrote: its key is quoted as written from there.
	optional_idx per_location;
	//! The clause the row came from: its source clause, its removal group, and whether a rewrite made it for the
	//! objective. A clause holds no repair group.
	DecideRowOrigin origin;
	DecideBetweenHalf between = DecideBetweenHalf::NONE;
	//! A rewrite added the row to define an auxiliary or to link its machinery. Such a row is rigid: no repair loosens
	//! it.
	bool structural = false;
	//! The auxiliary decision the row defines (an ABS's envelope, a body helper's definition, an L0 indicator's
	//! links): the auxiliary has no value where what its definition reads has none (D88). INVALID_INDEX for every
	//! other row.
	idx_t defines = DConstants::INVALID_INDEX;
	//! A MIN or MAX pushed its easy way (`max(e) <= K`, `min(e) >= K`) keeps its reducer: the linear form expands it
	//! to one row per member (RewriteMinMax).
	bool minmax_easy = false;
	//! The whole meaning of the row, a bound on one decision, is folded into the decision column's box
	//! (LogicalDecide::singleton_row_boxes), so it is not also a model row. The row stays, which EXPLAIN renders
	//! (FoldSingletonRows).
	bool folded_bound = false;
	//! A repair model loosens the folded bound through the clause's own rows, one per kept row or class (D28), not
	//! through one knob on the column.
	bool folded_clause_rows = false;
	//! The shape of the comparison, which the canonicalizer decides with the clause's scope; VALUE for an objective
	//! with a reducer under a Boolean decision.
	CanonicalConstraintClass shape = CanonicalConstraintClass::INVALID;
	//! The aggregate shape: the `by` key the clause's decision reducers share (index into LogicalDecide::keys), which
	//! groups its rows; INVALID_INDEX for the whole query and for every other shape.
	idx_t group_key = DConstants::INVALID_INDEX;
	//! What bound reduction (RewriteRhsAggregates) relied on for a bound that may differ between the rows of a group:
	//! the clause's shape, since only the aggregate shape takes the group's tightest value, and the comparison's
	//! direction, which says which value is the tightest. INVALID for any other bound. A rewrite may run after it, so
	//! a DEBUG build checks after every pass that both still hold (DecideOptimizer::VerifyBoundReads).
	CanonicalConstraintClass bound_read_shape = CanonicalConstraintClass::INVALID;
	ExpressionType bound_read_type = ExpressionType::INVALID;

	BoundDecideClause Copy() const {
		BoundDecideClause result;
		result.body = body ? body->Copy() : nullptr;
		result.scope = scope.Copy();
		result.per_location = per_location;
		result.origin = origin;
		result.between = between;
		result.structural = structural;
		result.defines = defines;
		result.minmax_easy = minmax_easy;
		result.folded_bound = folded_bound;
		result.folded_clause_rows = folded_clause_rows;
		result.shape = shape;
		result.group_key = group_key;
		result.bound_read_shape = bound_read_shape;
		result.bound_read_type = bound_read_type;
		return result;
	}

	//! The body and the `when`, each slot once, in that order.
	template <class CALLBACK>
	void EnumerateExpressions(CALLBACK &&callback) {
		if (body) {
			callback(body);
		}
		if (scope.when) {
			callback(scope.when);
		}
	}

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static BoundDecideClause Deserialize(Deserializer &deserializer);
};

//! One canonical comparison, flattened into terms.
struct DecideConstraint {
	vector<DecideTerm> lhs_terms;    // All additive terms from LHS
	unique_ptr<Expression> rhs_expr; // RHS expression (may contain aggregates)
	//! The bound reads a frame and may differ within a group, so each group's instance takes its tightest value (D85).
	//! The comparison carries FRAME_TIGHTEST_BOUND_TAG; a rewrite rebuilds its bound, and lowering replaces a bare
	//! frame.
	bool tightest_frame_bound = false;
	ExpressionType comparison_type; // COMPARE_LESSTHANOREQUALTO or GREATERTHANOREQUALTO
	bool lhs_is_aggregate = false;  // True if original LHS was an aggregate (e.g., SUM(...))
	bool was_minmax_easy = false;   // An easy-direction MIN/MAX kept its reducer (BoundDecideClause::minmax_easy):
	                                // one row per member, the bound reduced over each `by` group.
	idx_t abs_aux_idx = DConstants::INVALID_INDEX; // ABS auxiliary this envelope row bounds
	bool abs_is_pos_bound = false;                 // true=C1 (aux >= inner), false=C2 (aux >= -inner)
	//! The auxiliary this row defines (BoundDecideClause::defines): it has no value on a row where the decisions the
	//! row reads beside it have none (D88). INVALID_INDEX for every other row.
	idx_t defined_var = DConstants::INVALID_INDEX;
	//! The clause's `when` and `per`.
	DecideClauseScope scope;
	//! Aggregate shape: the `by` key its decision reducers share, which groups its rows (index into
	//! LogicalDecide::keys); INVALID_INDEX for `by ()`. Read off the comparison, not its scope. Every bound-side
	//! reducer reads its own `by` group of the kept rows.
	idx_t group_key = DConstants::INVALID_INDEX;
	ConstraintKind kind = ConstraintKind::USER_PARAMETER;
	//! The clause in LogicalDecide::constraint_sources and the atomic DROP group, independent of display and elastic
	//! grouping provenance.
	DecideRowOrigin origin;
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
	//! The objective reads only the rows its `when` keeps, and a term beside its reducers
	//! counts once, read on the first kept row.

	//! The objective's terms of degree 2 (DecideProductGroup), summed into one Q.
	vector<DecideProductGroup> product_groups;

	DecideObjective() = default;
};

//! A value a frame selector reads, which stage 08 computes as a column beside the input's:
//! every expression that reads a frame reads it as chunk column
//! `DecideFrameReads::column_base + j` for entry j, so no evaluator learns a new
//! node. `value` is evaluated on the rows `kind` names only, which is what makes an unused
//! fallback never evaluated (language/guide.md §5.6). A frame with nothing to read at a row
//! reads NULL there, which is no value (D84): the comparison's instance is skipped, the
//! reducer's member dropped, the objective's term dropped -- as for a NULL the row holds.
//! A frame over data reads one value:
//!   VALUE    — a point: `value`, its payload, at the row the frame selects for the row;
//!              `fallback`, its `else`, at the row itself where it selects none; NULL where
//!              neither stands
//!   REDUCED  — a range: `value`, its marker -- the frame's reducer over its payload -- over
//!              the values the row's range reads, `fallback` read at the row once per missing
//!              position; a NULL among them skipped, as SQL's aggregate skips it; NULL where it
//!              reads nothing
//! A frame over decisions reads its terms' coefficients:
//!   READ     — `value` at the row the frame selects for the row; where it selects none, 0
//!              when the frame has an `else` (which stands in), else NULL
//!   FALLBACK — `value` at the row itself where a position the frame requests is missing and
//!              it has an `else`; 0 everywhere else. A range has these too, over data or
//!              decisions: its helper's definition and its signature field read them
//!   DEFINED  — TRUE where the frame has a value: a point's every READ and FALLBACK column
//!              holds one, a range's window reads something; NULL elsewhere (BOOLEAN; `value`
//!              is null): what a reducer's NULL test reads for a frame over decisions in its
//!              argument (DecideCanonicalizer::SkipNullData)
//! A reducer over data whose argument reads a frame (LogicalDecide::frame_aggregates) is one column of its own, after
//! every frame's, computed once they are:
//!   AGGREGATE — `value`, the reducer over the rows its filter keeps, its argument reading frame columns: on each row
//!              its group's value of `key`, in the reducer's type, each class of `class_key` counted once per
//!              signature; NULL on a row in no group, and where the group has none to reduce (`count`: 0). `frame` is
//!              INVALID_INDEX
struct DecideFrameColumn {
	enum class Kind : uint8_t { VALUE, READ, FALLBACK, DEFINED, REDUCED, AGGREGATE };
	Kind kind = Kind::READ;
	idx_t frame = DConstants::INVALID_INDEX;
	unique_ptr<Expression> value;
	//! VALUE, REDUCED: the frame's `else`, of the payload's type; null when it has none.
	unique_ptr<Expression> fallback;
	LogicalType type;
	//! AGGREGATE: its path key and its own `per` key, into LogicalDecide::keys, and the reducer as written
	//! (LogicalDecide::FrameAggregate).
	idx_t key = DConstants::INVALID_INDEX;
	idx_t class_key = DConstants::INVALID_INDEX;
	string text;
};

//! The values the frame selectors read, as columns after the input's, which stage 05's lowering makes of every frame
//! read (LowerDecideTree) and the tree holds (DecideTree::frame_reads).
struct DecideFrameReads {
	//! The columns: the first is chunk column `column_base`, the number of columns the input has.
	vector<DecideFrameColumn> columns;
	idx_t column_base = 0;
	//! Per frame, its `else` as a signature keys it where an anchor reaches it (D82): the
	//! FALLBACK columns of its terms (into `columns`), and the decisions it reads at the
	//! row itself. Both empty for a frame with no `else`.
	vector<vector<idx_t>> fallback_columns;
	vector<vector<idx_t>> fallback_decisions;
};

} // namespace duckdb
