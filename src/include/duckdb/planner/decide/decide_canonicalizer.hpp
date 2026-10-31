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
// `MAX(x*v)`, and a filtered `sum(when c: x)` are all simply decision-bearing
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
// SHAPE IS DECIDED AT THIS BOUNDARY. After rebuilding, ClassifyScoped reads the
// decision side as one of three shapes: ROWS (no decision reducer: one row per kept
// row or class), AGGREGATE (decision reducers sharing one `by` key beside query-wide
// terms: one row per group) or VALUE (a decision reducer beside a per-row or keyed
// decision, a row-varying factor or a second `by` key: the value rewrite replaces each
// reducer by a helper decision). Only a decision on the bound side and the shapes
// ClassifyScoped names in its refusal are rejected.
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
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {

class ClientContext;
class BoundAggregateExpression;
class BoundComparisonExpression;
class BoundConjunctionExpression;
struct DecideSourceNames;
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

//! The semantic row shape of a canonical DECIDE comparison. VALUE is a comparison whose
//! decision side mixes a decision reducer with something the aggregate shape cannot hold
//! -- a per-row or keyed decision, a factor that varies by row, a second `by` key, a
//! reducer inside another expression: the value rewrite (stage 05) replaces each decision
//! reducer by a helper decision defined once per `by` group, after which the comparison
//! is PER_ROW. INVALID means the bound side bears a decision, or ClassifyScoped refused
//! the shape.
enum class CanonicalConstraintClass : uint8_t { PER_ROW, AGGREGATE, VALUE, INVALID };

//! The scope a clause is written under: the WHEN condition and the `per K:` wrapper
//! (PER_CLASS_TAG) that enclose it, if any.
struct DecideEnclosingScope {
	const Expression *when = nullptr;
	const BoundConjunctionExpression *per = nullptr;
};

//! What building the scope checks of the user's clauses reads besides the bound tree,
//! and the checks it builds (LogicalDecide::scope_checks), in clause order.
struct DecideScopeChecks {
	const vector<DecideGroupKey> &group_keys;
	const vector<EntityScopeInfo> &entity_scopes;
	const vector<string> &source_fragments;
	//! A relation's alias by table index, so a message names a column `D.depotID`.
	unordered_map<idx_t, string> relations;
	vector<DecideScopeCheck> checks;
};

//! What building the variant signatures of the user's clauses reads and writes
//! (LogicalDecide::signatures).
struct DecideSignatures {
	const vector<DecideGroupKey> &group_keys;
	//! A relation-qualified reducer whose body has a field gets a scope of its own,
	//! appended here (EntityScopeInfo::signature), and its tag names it.
	vector<EntityScopeInfo> &entity_scopes;
	//! The frames a field may name (LogicalDecide::frames).
	const vector<DecideFrameSpec> &frames;
	vector<DecideSignature> signatures;
	//! The objective's, or INVALID_INDEX.
	idx_t objective = DConstants::INVALID_INDEX;
};

//! What placing the frame selectors of the user's clauses writes (PlaceFrames): the
//! frames, the path keys their timelines are split by (appended to `group_keys` when new),
//! and their FRAME_PARTITION checks (into `scope`).
struct DecideFramePlacement {
	vector<DecideFrameSpec> &frames;
	vector<DecideGroupKey> &group_keys;
	DecideScopeChecks &scope;
	//! Which frames are placed: each stands in exactly one place.
	vector<bool> placed;
};

class DecideCanonicalizer {
public:
	//! Whether `expr` holds a reducer anywhere, itself included.
	static bool ContainsReducer(const Expression &expr);
	//! Whether `expr` holds one value for the whole query, decisions included: the test
	//! that puts a term beside a clause's reducers in its aggregate row (ClassifyScoped).
	//! Unlike IsQueryWideConstant, it admits query-wide (`per ()`) decisions while
	//! rejecting row and keyed ones and row-varying columns.
	bool IsQueryWideExpression(const Expression &expr) const;

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

	//! Canonicalize a whole SUCH THAT tree: AND-conjunctions plus the `when` and `per K:`
	//! wrappers. Descends only into the constraint child of a wrapper, never into its
	//! condition or key columns. Pure -- `constraints` is untouched.
	unique_ptr<Expression> CanonicalizeTree(const Expression &constraints) const;

	//! The scope checks of every clause of the user's SUCH THAT tree, appended to `scope`
	//! in clause order: each `when` keeps a row (D4), and each reducer's own `when` a row of
	//! every `by` group (D8). Read-only, from the clauses as bound. Called once, by the
	//! planner; a rewrite's clause (AddConstraint) never builds any.
	void BuildConstraintScopeChecks(const Expression &constraints, DecideScopeChecks &scope) const;

	//! The scope checks of the objective, built from the bound objective before
	//! CanonicalizeObjective: its `when` keeps a row, and each reducer reads a row.
	void BuildObjectiveScopeChecks(const Expression &objective, DecideScopeChecks &scope) const;

	//! The variant signature of every scoped expression of the canonical SUCH THAT tree
	//! with a field (syntax_reference.md §7.1; D64): each `per K:` clause, its wrapper
	//! tagged with it (SIGNATURE_TAG_PREFIX), and each relation-qualified reducer, given a
	//! scope of its own (EntityScopeInfo::signature). Its fields are the tuple terms and
	//! decisions outside reducers and the reducers' `by` keys, read on the canonical tree
	//! so the data terms of one additive context are one field (D77). Called once, by the
	//! planner, right after CanonicalizeTree.
	void BuildConstraintSignatures(Expression &constraints, DecideSignatures &out) const;

	//! The objective's signature, `out.objective` -- it is produced once (D66), so
	//! execution refuses a field with a second value -- and its qualified reducers', after
	//! CanonicalizeObjective.
	void BuildObjectiveSignatures(Expression &objective, DecideSignatures &out) const;
	//! Where every frame selector of the canonical SUCH THAT tree stands
	//! (syntax_reference.md §5.6): the rows its timelines read, which are its scoped
	//! expression's path rows (§5.5); the path's `by` keys they are split by before
	//! `within`; the comparison it is written in; and whether it is inside a reducer
	//! (DecideFrameSpec::rows, scope_key, source_clause_id, reduced). Builds the
	//! FRAME_PARTITION check (D69) of each frame a `per` key reads directly -- a clause's
	//! `per K:`, or a reducer's own `per` -- unless the key determines its timeline already.
	//! Refuses a reducer that may be absent under `abs` inside another reducer over
	//! decisions (D82). Called once, by the planner, after CanonicalizeTree.
	void PlaceFrames(const Expression &constraints, DecideFramePlacement &placement) const;
	//! The same for the objective, after CanonicalizeObjective. Its root has one class
	//! (D66), whose fields the objective's signature checks, so only a reducer's own `per`
	//! gets a FRAME_PARTITION check there.
	void PlaceObjectiveFrames(const Expression &objective, DecideFramePlacement &placement) const;

	//! Whether a frame in `reducer`'s argument, outside the reducers nested in it, may
	//! select nothing, and so drop a member (syntax_reference.md §5.6): the
	//! MEMBER_FRAME_TAG_PREFIX tags TagDecideMemberFrames stamped before canonicalization.
	static bool DropsMembers(const BoundAggregateExpression &reducer);

	//! Quote expressions in the scope refusals as the query writes them, from
	//! these names, rather than through the bound tree's own names. Set by the planner,
	//! which alone has them; `names` must outlive the canonicalizer.
	void QuoteMessagesWith(const DecideSourceNames &names) {
		message_names = &names;
	}

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
	//! Which clause a term was written in. It picks the wording of a rejection -- a
	//! message that calls the objective a constraint sends the user to the wrong line of
	//! their query -- and whether a row-varying factor on a reducer is a tuple term (a
	//! constraint, whose comparison then takes the VALUE shape) or refused (the objective,
	//! produced once).
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
	//! binary `-`, unary `-`, and binder-inserted casts over decision algebra, and
	//! distributes a factor known before the query runs over an additive child
	//! (`2 * (min(x) - 1)` is `2 * min(x) - 2 * 1`), carried down as `factor`; every
	//! other node, including a data-only cast, is a term boundary.
	void Decompose(const Expression &expr, int sign, vector<Atom> &out, Clause clause = Clause::CONSTRAINT,
	               const Expression *factor = nullptr, bool factor_divides = false) const;

	//! Merge like decision atoms -- the same term, each under a factor known before the
	//! query runs -- into one atom scaled by the sum of their signed factors: `x + x` is
	//! `2 * x`, `x - 3 * x` is `-2 * x`, `2 * min(x) + min(x)` is `3 * min(x)`. A group
	//! whose factors cancel is left as written. Returns whether anything merged.
	bool MergeLikeAtoms(vector<Atom> &atoms) const;

	//! The one factor `outer` stands for when applied outside a term already scaled by
	//! `inner`: `M`, `D` or `M / D`, in the spelling PeelScale emits. Either may be null.
	unique_ptr<Expression> ComposeScale(const Expression *outer, bool outer_divides, unique_ptr<Expression> inner,
	                                    bool inner_divides, bool &out_divides) const;

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
	//! In a constraint a factor that varies by row is a tuple term of the row's constraint
	//! (syntax_reference.md §6.4): it is peeled like any other and the comparison takes the
	//! VALUE shape, where the reducer becomes a helper decision the row's factor multiplies.
	const Expression &PeelScale(const Expression &expr, unique_ptr<Expression> &out_scale, bool &out_divides,
	                            Clause clause = Clause::CONSTRAINT) const;

	//! True when `expr` evaluates to a single value for the entire query, so it may
	//! scale a reducer. Constants qualify by folding; a column ref qualifies only by
	//! being listed in `query_wide_table_indexes`, because after flattening a
	//! `(SELECT max(w) FROM p)` is shape-identical to `weight` -- and, importantly, to
	//! a CORRELATED subquery, which is per-row and must not qualify.
	bool IsQueryWideConstant(const Expression &expr) const;

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
	void ValidateCanonicalTree(const Expression &constraints) const;
	void ValidateCanonicalComparison(const BoundComparisonExpression &comparison) const;
	void VerifyCanonicalTree(const Expression &constraints) const;
	//! Wrapper-aware structural half of VerifyCanonicalObjective. Runs before the
	//! fixed-point check so a malformed body is reported as the invariant it breaks,
	//! rather than as whatever CanonicalizeObjective would have thrown re-reading it.
	void VerifyCanonicalObjectiveBody(const Expression &objective) const;
	void VerifyCanonicalComparison(const BoundComparisonExpression &comparison) const;

	unique_ptr<Expression> CanonicalizeTreeInternal(const Expression &constraints) const;
	void BuildConstraintScopeChecks(const Expression &constraints, DecideEnclosingScope enclosing,
	                                DecideScopeChecks &scope) const;

	//! The shape of a comparison's decision side (syntax_reference.md §6.4): ROWS has no
	//! decision reducer; AGGREGATE has decision reducers sharing one `by` key under
	//! whole-query factors with only query-wide terms beside them (one model row per group);
	//! VALUE needs the value rewrite; UNSUPPORTED is refused with `refusal`. `group_key` is
	//! the AGGREGATE shape's shared key, INVALID for `by ()`.
	enum class ScopedShape : uint8_t { ROWS, AGGREGATE, VALUE, UNSUPPORTED };
	ScopedShape ClassifyScoped(const BoundComparisonExpression &comparison, idx_t &group_key,
	                           string &refusal) const;
	//! Whether a comparison's decision side takes the composed MIN/MAX formulation
	//! (RewriteComposedMinMaxInConstraint): a MIN or MAX over decisions beside another
	//! term, or alone under a factor whose sign is not known before the query runs.
	bool TakesComposedMinMax(const vector<Atom> &atoms) const;

	//! The scope checks of one clause, `leaf` its
	//! comparison (or other constraint) as bound.
	void BuildClauseScopeChecks(const Expression &leaf, DecideEnclosingScope enclosing,
	                            DecideScopeChecks &scope) const;
	//! The checks of one reducer and of the reducers nested in it: a row in every group of
	//! its path key among the rows its path keeps (D8, syntax_reference.md §5.5).
	void BuildReducerScopeChecks(const BoundAggregateExpression &reducer, const DecideReducerPath &path,
	                             DecideScopeChecks &scope) const;

	//! The signature fields `expr` reads (D64, D77), appended to `fields` unless there or in
	//! `key` already: a decision by instance, a reducer's `by` key, and data by value -- the
	//! data terms of one additive context summed into one field.
	void CollectSignatureFields(const Expression &expr, const DecideSignatures &out, Clause clause,
	                            const vector<unique_ptr<Expression>> &key,
	                            vector<DecideSignatureField> &fields) const;
	idx_t AddSignature(DecideSignatures &out, const Expression *rows, vector<unique_ptr<Expression>> key,
	                   vector<DecideSignatureField> fields, string text, idx_t source_clause_id) const;
	//! Gives every relation-qualified reducer in `expr` whose body has a field its
	//! signature and a scope of its own, counted over the rows its path keeps (syntax_reference.md §5.5).
	void SignQualifiedReducers(Expression &expr, const DecideReducerPath &path, Clause clause,
	                           idx_t source_clause_id, DecideSignatures &out) const;
	void BuildConstraintSignatures(Expression &expr, const Expression *when, DecideSignatures &out) const;

	//! The `per` key a scoped expression is read under, for its frames' FRAME_PARTITION
	//! checks: its expressions, how a message names each, and the key as written.
	struct FramePerKey {
		vector<unique_ptr<Expression>> key;
		vector<string> names;
		string text;
	};
	void PlaceClauseFrames(const Expression &expr, DecideEnclosingScope enclosing,
	                       DecideFramePlacement &placement) const;
	//! The frames of `expr` along `path`: one outside every reducer of its comparison
	//! stands at the comparison's root, one in a reducer's argument reads that reducer's
	//! members -- `reducer` is the innermost reducer around `expr`, null at the root. `per`
	//! is the key of the scoped expression `expr` is the body of; null for `per row`, which
	//! no frame's timeline can span. `under_abs` is the `abs` `expr` stands under inside
	//! `reducer`, if any: a reducer there that may be absent is refused.
	void PlaceFramesIn(const Expression &expr, const DecideReducerPath &path, const BoundAggregateExpression *reducer,
	                   idx_t source_clause_id, const FramePerKey *per, DecideFramePlacement &placement,
	                   const Expression *under_abs = nullptr) const;
	//! The first frame that may leave `reducer` -- a reducer over decisions -- with no
	//! member: one of its own member frames, else one of a reducer nested in it that may
	//! be absent; INVALID_INDEX when none can.
	idx_t FirstAbsenceFrame(const BoundAggregateExpression &reducer) const;
	//! The key of a reducer's own `per` (or of the colon form), when it has one.
	bool ReducerPerKey(const BoundAggregateExpression &reducer, const vector<EntityScopeInfo> &entity_scopes,
	                   FramePerKey &out) const;

	//! Rebuild an additive chain from terms, negating every sign when `negate`
	//! is set (used for the terms that crossed the relation).
	unique_ptr<Expression> BuildAdditive(const vector<Atom> &atoms, bool negate) const;
	//! Whether `atoms` is one negated reducer (`-max(e)`, `-(2 * max(e))`).
	bool IsLoneNegatedReducer(const vector<Atom> &atoms) const;
	//! Whether `side` spells one term with signs around it (`-(-max(e))`): its canonical
	//! form is the bare term, which only a rebuild gives.
	bool IsSignedSpine(const Expression &side, const vector<Atom> &atoms) const;
	//! Moves a negated reducer's sign into its factor: `-1 * max(e)`, `-(2) * max(e)`.
	void FoldNegationIntoFactor(Atom &atom) const;

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
	//! See QuoteMessagesWith; null quotes through the bound tree (UserFacingName).
	optional_ptr<const DecideSourceNames> message_names;

	//! `expr` as a scope refusal quotes it.
	string Quote(const Expression &expr) const;
	//! `expr` by its written text when the query recorded it, else by its bound name.
	string QuoteAsWritten(const Expression &expr) const;
};

} // namespace duckdb
