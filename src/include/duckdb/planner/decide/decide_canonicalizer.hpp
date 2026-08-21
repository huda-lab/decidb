//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/decide/decide_canonicalizer.hpp
//
// The single home for DECIDE constraint and objective canonicalization.
//===----------------------------------------------------------------------===//
//
// CANONICAL FORM
// --------------
// A bound DECIDE constraint is canonical when it is a BoundComparisonExpression
// `L <op> R` where R contains no decision-variable reference, and every
// decision-bearing term lives in L. Canonicalization decides that shape once;
// nothing downstream re-decides it.
//
// THE RULE THAT MAKES THE TRANSFORM WORK: canonicalization NEVER opens a term
// algebraically. It decomposes each side additively and asks one question of each
// resulting term -- "does it reference a decision variable?" -- then moves terms
// across the relation accordingly. It does not expand, combine like terms, fold
// constants, or distribute. Validation subsequently inspects reducer placement,
// variable scope, and query-wide provenance without rewriting the term.
//
// That restriction is the whole design. A quadratic `POWER(x-t,2)`, a composed
// `MAX(x*v)`, and a filtered `SUM(x) WHEN c` are all simply decision-bearing
// terms here, indistinguishable from `x`. The symbolic layer this replaced had to
// carve out structural bypasses -- quadratic bodies, nested aggregates, opaque
// placeholders for ABS/MIN/MAX/subqueries -- precisely because it opened terms to
// do its algebra. A pass that never opens a term has nothing to bypass, and can
// therefore be TOTAL. Totality is what lets this be the single home: a pass that
// may decline forces every consumer downstream to handle the declined case, which
// is precisely how canonicalization came to be spread across five sites.
//
// PURITY: Canonicalize* take their input by const reference and return a new
// tree, leaving the input untouched. This is deliberate and load-bearing for a
// planned EXPLAIN feature that renders as-written / canonical / rewritten side
// by side (see context/descriptions/03_expressivity/explain/done.md): with a pure
// function both forms are live at the call site, so that feature is a rendering
// change rather than a re-plumbing job.
//
// CALL SITES. One boundary, two clauses, two producers -- and there must never be
// a third producer:
//   1. Binder::PlanSelectNode       -- everything the user wrote
//   2. LogicalDecide::AddConstraint -- constraints the optimizer emits
//      LogicalDecide::SetObjective  -- objectives the optimizer rewrites
//
// OBJECTIVES SHARE THIS BOUNDARY. An objective is one SIDE of a comparison: no
// relation to orient, no bound to separate, so the two side-partitioning rules do
// not apply to it. Every other rule does, and CanonicalizeObjective reuses the same
// Decompose / PeelScale / BuildAdditive machinery rather than restating it. What it
// adds is peeling additive constants into objective_constant_offset.
//
// CAST AUTHORSHIP IS SETTLED BEFORE THIS PASS. The parsed boundary rejects every
// user-authored cast whose child contains decision algebra. Bound casts over a
// decision-bearing subtree are therefore DuckDB's type-reconciliation noise and
// are transparent on the additive spine. A data-only cast is an ordinary SQL
// computation and remains an atomic term; this pass never distributes or reapplies
// it. BindOp may add new reconciliation casts while rebuilding, so decision-aware
// cast descent is also required for the pass to remain a fixed point.
//
// SCALE IS PART OF THE ATOM, NOT OF THE TREE SHAPE. The additive spine peels a
// query-wide factor outward off a decision-bearing reducer, so a term is
// {sign, scale, expr} and every consumer reads one spelling: `scale * term`, with
// the scale on the left (or `term / scale` for a division, which is left as a
// division rather than reciprocated). This replaced a parsed-level fold that
// pushed the factor INWARD, which was unsound for MIN/MAX under a negative factor.
// Peeling is also where the scale-specific rule "a factor on a reducer must be
// one value for the whole query" is enforced.
//
// K3 IS ENFORCED AT THIS BOUNDARY. After rebuilding, validation classifies the
// whole comparison as per-row or aggregate using reducer placement plus DECIDE
// variable scope. Row/entity-varying algebra beside a reducer is rejected here;
// query-wide scalar decisions remain legal top-level aggregate terms.
//
// K1 is now load-bearing rather than advisory. The physical operator's duplicate
// partition logic (its per-row branch and lhs_offset_expr) was deleted at
// the canonicalization refactor after being verified unreachable across the golden corpus
// and the full suite; what stands in its place is a check that throws when a
// decision variable reaches the RHS. So a rewrite that breaks K1 now fails
// loudly instead of being silently absorbed -- but it does fail. Any new
// optimizer pass that mutates a constraint in place must go through
// LogicalDecide::AddConstraint (which canonicalizes) rather than editing the
// tree directly.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class ClientContext;
class BoundAggregateExpression;
class BoundComparisonExpression;
class BoundColumnRefExpression;
class BoundFunctionExpression;

//! One-level reducer scale consumed after canonicalization. Exactly two spellings
//! reach any consumer -- `factor * AGG` and `AGG / factor` -- because BuildAdditive
//! re-attaches every peeled factor on the LEFT. Objectives used to additionally admit
//! `AGG * factor`, which is what an unnormalized clause looks like; they go through
//! CanonicalizeObjective now, so that third spelling no longer exists.
struct ScaledAggregateMatch {
	const BoundAggregateExpression *aggregate = nullptr;
	const Expression *scale = nullptr;
	const BoundFunctionExpression *function = nullptr;
	bool divides = false;
};

bool TryMatchScaledAggregate(const Expression &expr, idx_t decide_index, ScaledAggregateMatch &result);

//! The semantic row shape of a canonical DECIDE comparison. INVALID means the
//! tree mixes reduced values with row/entity-varying decision algebra and cannot
//! denote either one constraint per row or one reduced constraint.
enum class CanonicalConstraintClass : uint8_t { PER_ROW, AGGREGATE, INVALID };

class DecideCanonicalizer {
public:
	//! Provenance-only form: no table-index evidence is available, so arbitrary column
	//! refs are never promoted to query-wide. Explicit QUERY_WIDE_VALUE_TAG metadata is
	//! still honored. Variable scopes remain available for homogeneity validation.
	//! Used by LogicalDecide::AddConstraint for optimizer output.
	DecideCanonicalizer(ClientContext &context, idx_t decide_index,
	                    vector<DecideVarScopeInfo> variable_scopes);

	//! Strict form: `query_wide_table_indexes` are the table indexes that UNCORRELATED
	//! scalar subqueries flattened into, collected in plan_select_node.cpp at the only
	//! point where correlation is still visible. A column ref counts as query-wide iff
	//! its table index is in that set.
	//!
	//! The direction matters and is deliberately FAIL-SAFE: anything not positively
	//! marked is treated as row-varying. The reverse framing (a deny-list of the
	//! relation's own bindings) reads the same for `weight` but silently admits a
	//! CORRELATED subquery, which also flattens onto a fresh table index while being
	//! per-row by construction.
	//!
	//! `correlated_subquery_table_indexes` is carried purely so the rejection can name
	//! what the user wrote. A flattened correlated subquery is a column ref literally
	//! named "SUBQUERY", and reporting that -- or suggesting `SUM(x * SUBQUERY)` --
	//! would name an object the user never typed.
	DecideCanonicalizer(ClientContext &context, idx_t decide_index,
	                    vector<DecideVarScopeInfo> variable_scopes,
	                    unordered_set<idx_t> query_wide_table_indexes,
	                    unordered_set<idx_t> correlated_subquery_table_indexes = {});

	//! Canonicalize a whole SUCH THAT tree: AND-conjunctions plus WHEN/PER
	//! wrappers. Descends only into the constraint child of a wrapper, never
	//! into its condition or PER columns. Pure -- `constraints` is untouched.
	unique_ptr<Expression> CanonicalizeTree(const Expression &constraints) const;

	//! Canonicalize a single comparison. Pure -- `comparison` is untouched.
	unique_ptr<Expression> CanonicalizeComparison(const Expression &comparison) const;

	//! Canonicalize a bound DECIDE objective. Pure -- `objective` is untouched.
	//!
	//! An objective is structurally ONE SIDE of a comparison. There is no relation to
	//! orient and no bound to separate, so the two side-partitioning rules (decisions
	//! left, data right) have no analogue here. Everything else this boundary owns
	//! applies unchanged -- wrapper recursion, additive decomposition, decision-aware
	//! cast transparency, and one spelling for a reducer scale -- and is SHARED with
	//! CanonicalizeComparison rather than reimplemented, which is the whole reason
	//! objectives get a boundary instead of their own normalizer.
	//!
	//! Additive decision-free terms are folded into `out_constant_offset`, which is
	//! ADDED to rather than assigned, so re-canonicalizing through
	//! LogicalDecide::SetObjective accumulates correctly. Such a term shifts the
	//! objective without moving its argmax/argmin, but must be added back to report an
	//! objective VALUE.
	//!
	//! An objective with no decision content at all is returned unchanged: the
	//! optimizer's composed MIN/MAX rewrite deliberately installs a constant
	//! placeholder and supplies coefficients from its own spec.
	unique_ptr<Expression> CanonicalizeObjective(const Expression &objective, double &out_constant_offset) const;

	//! Classify an already-canonical comparison using the same scope rules as
	//! planning validation. Physical extraction uses this instead of re-deciding
	//! aggregate-versus-per-row shape from the mere presence of an aggregate.
	CanonicalConstraintClass ClassifyCanonicalComparison(const Expression &comparison) const;

	//! Assert the structural invariant owned by this boundary. This is deliberately
	//! non-mutating and throws InternalException: once binding/canonicalization has
	//! accepted a constraint, a non-canonical tree is an engine bug rather than a
	//! user-input error.
	//!
	//! DEBUG-ONLY, like LogicalOperator::Verify: the body re-canonicalizes the whole
	//! tree to check idempotency, which is too expensive to pay on every DECIDE query
	//! in release. Call sites call it unconditionally; in a release build it compiles
	//! to an empty function. Build `debug` or `relassert` to exercise it.
	void VerifyCanonical(const Expression &constraints) const;

	//! The objective counterpart of VerifyCanonical. Step 6 shipped the constraint
	//! verifier with objectives explicitly not inspected, because objectives had no
	//! canonical form to check against; CanonicalizeObjective gives them one.
	//! Non-mutating, and throws InternalException for the same reason: past this
	//! boundary a non-canonical objective is an engine bug, not user input.
	//!
	//! DEBUG-ONLY -- see VerifyCanonical.
	void VerifyCanonicalObjective(const Expression &objective) const;

private:
	//! Which clause a term was written in. This affects ONLY the wording of a
	//! rejection -- the scale rule and the additive decomposition are identical in
	//! both -- but a message that calls the objective a constraint sends the user to
	//! the wrong line of their query.
	enum class Clause : uint8_t { CONSTRAINT, OBJECTIVE };

	//! Where a term is required to sit.
	//!   LEFT    decision-bearing -- must be on the model side
	//!   RIGHT   everything else: constants, per-row columns, and data-only
	//!           reducers alike -- must be on the bound side
	//!
	//! There used to be a third case, NEUTRAL, for a data-only reducer: it carries
	//! no decision variable so it need not be on the left, but it collapses rows to
	//! one value so it is a legitimate bound and need not be on the right either.
	//! That was scaffolding for a right-hand side that could not evaluate one
	//! (D8). B.5 built the evaluator, so K1 now means what it should -- decisions
	//! left, data right -- rather than the half-rule that left data floating on
	//! both sides.
	enum class Placement : uint8_t { LEFT, RIGHT };

	//! One additive term of a comparison side. `expr` is borrowed from the input
	//! tree and is only ever copied, never mutated. Binder-inserted casts over the
	//! decision spine are transparent; data-only casts remain inside `expr`.
	//!
	//! `scale` is a factor peeled off a reducer-bearing term: `2 * SUM(x)` becomes
	//! {scale=2, expr=SUM(x)} rather than one opaque term. Peeling moves the factor
	//! OUTWARD and is the opposite of the fold it replaces -- folding inward
	//! (`2 * MAX(x)` -> `MAX(2*x)`) is unsound for MIN/MAX under a negative factor,
	//! since `MAX(-2x)` is `-2*MIN(x)`. Crucially, peeling does not open the term:
	//! this pass still cannot tell `SUM(x)` from `POWER(x-t,2)`, so totality survives.
	//!
	//! An Atom is move-only because `scale` owns its expression. Nested factors
	//! (`2 * (3 * SUM(x))`) compose into a single new node that exists nowhere in the
	//! input tree, so the slot cannot borrow the way the other members do.
	struct Atom {
		int sign;
		const Expression *expr;
		Placement placement;
		//! The composed factor on `expr`, or nullptr. Owned: see the note above.
		unique_ptr<Expression> scale;
		//! false: the term rebuilds as `scale * expr`. true: as `expr / scale`.
		bool scale_divides = false;
	};

	//! Split an expression into signed additive terms. Descends through `+`,
	//! binary `-`, unary `-`, and binder-inserted casts over decision algebra;
	//! every other node, including a data-only cast, is a term boundary.
	void Decompose(const Expression &expr, int sign, vector<Atom> &out,
	               Clause clause = Clause::CONSTRAINT) const;

	//! If `expr` is a decision-bearing reducer with factors on it, split them out into
	//! `out_scale` / `out_divides` and return the bare reducer term. Returns `expr`
	//! unchanged when there is nothing to peel.
	//!
	//! Peels to exhaustion, not one level: `2 * (3 * SUM(x))` and `(SUM(x) / 2) / 3`
	//! yield ONE factor each. That is what totality means here -- every consumer
	//! downstream matches a single-level `factor * AGG` / `AGG / factor` shape, so a
	//! partially peeled term is a term nothing downstream can read. Multipliers and
	//! divisors compose into one node (`M`, `D`, or `M / D`) rather than a chain,
	//! which reassociates in double arithmetic; that is deliberate and accepted --
	//! the model is double end to end and the difference is last-ULP.
	//!
	//! Throws when a factor is not a legal scale, which is the single place that
	//! judgement is made. A factor multiplying a reducer must be ONE value for the
	//! whole query: `weight * SUM(x)` asks which row's weight scales a number that has
	//! no row, and `s * SUM(x)` is a product of two decisions (bilinear), not a scale.
	const Expression &PeelScale(const Expression &expr, unique_ptr<Expression> &out_scale, bool &out_divides,
	                            Clause clause = Clause::CONSTRAINT) const;

	//! True when `expr` evaluates to a single value for the entire query, so it may
	//! scale a reducer. Constants qualify by folding; a column ref qualifies only by
	//! being listed in `query_wide_table_indexes`, because after flattening a
	//! `(SELECT max(w) FROM p)` is shape-identical to `weight` -- and, importantly, to
	//! a CORRELATED subquery, which is per-row and must not qualify.
	bool IsQueryWideConstant(const Expression &expr) const;
	//! Query-wide counterpart used for top-level aggregate terms. Unlike
	//! IsQueryWideConstant, this admits scalar DECIDE variables while rejecting
	//! row/entity decisions and ordinary row-varying columns.
	bool IsQueryWideExpression(const Expression &expr) const;

	//! Recompute semantic provenance for the complete canonical bound. A stale root
	//! classification is removed before inspecting the rebuilt tree; only an RHS whose
	//! every component is query-wide receives QUERY_WIDE_BOUND_TAG.
	unique_ptr<Expression> FinalizeBoundProvenance(unique_ptr<BoundComparisonExpression> comparison) const;

	//! True when `expr` is what a CORRELATED scalar subquery flattened into. Affects
	//! only the wording of a rejection, never the decision.
	bool IsCorrelatedSubqueryRef(const Expression &expr) const;

	//! The one question this pass asks of a term. Never inspects further.
	Placement Classify(const Expression &expr) const;

	bool ReferencesDecideVar(const Expression &expr) const;
	static bool ContainsReducer(const Expression &expr);
	bool FindNonScalarDecideVar(const Expression &expr, idx_t &out_var_idx,
	                            const BoundColumnRefExpression *&out_ref) const;
	void ValidateCanonicalTree(const Expression &constraints) const;
	void ValidateCanonicalComparison(const BoundComparisonExpression &comparison) const;
	void VerifyCanonicalTree(const Expression &constraints) const;
	//! Wrapper-aware structural half of VerifyCanonicalObjective. Runs before the
	//! fixed-point check so a malformed body is reported as the invariant it breaks,
	//! rather than as whatever CanonicalizeObjective would have thrown re-reading it.
	void VerifyCanonicalObjectiveBody(const Expression &objective) const;
	void VerifyCanonicalComparison(const BoundComparisonExpression &comparison) const;
	CanonicalConstraintClass ClassifyCanonicalTree(const Expression &constraints) const;
	unique_ptr<Expression> CanonicalizeTreeInternal(const Expression &constraints) const;

	//! Rebuild an additive chain from terms, negating every sign when `negate`
	//! is set (used for the terms that crossed the relation).
	unique_ptr<Expression> BuildAdditive(const vector<Atom> &atoms, bool negate) const;

	unique_ptr<Expression> BindOp(const string &name, unique_ptr<Expression> left,
	                              unique_ptr<Expression> right) const;
	//! Unary form, for the leading negative term of a rebuilt spine.
	unique_ptr<Expression> BindOp(const string &name, unique_ptr<Expression> operand) const;
	unique_ptr<Expression> BindOp(const string &name, vector<unique_ptr<Expression>> children) const;

	ClientContext &context;
	idx_t decide_index;
	//! Scope assignment indexed by decide-column index. Missing entries fail safe
	//! as row-scoped, which is also the historical default for old plans.
	vector<DecideVarScopeInfo> variable_scopes;
	//! Table indexes that uncorrelated scalar subqueries flattened into. Meaningful
	//! only when `judge_column_refs` is set.
	unordered_set<idx_t> query_wide_table_indexes;
	//! Table indexes that CORRELATED scalar subqueries flattened into. Never affects
	//! the decision (they are row-varying by construction, and would be rejected by the
	//! fail-safe default anyway) -- only the wording of the rejection.
	unordered_set<idx_t> correlated_subquery_table_indexes;
	//! False when the pass was given no flattening evidence. In that mode an arbitrary
	//! column ref is never guessed to be query-wide; only explicit semantic tags qualify.
	bool judge_column_refs = false;
};

} // namespace duckdb
