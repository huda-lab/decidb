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
// across the relation accordingly. It does not expand a product of terms or fold
// constants. Two steps give a bound one spelling: a factor known before the query
// runs distributes over an additive spine (`2 * (x - 1)` is `2 * x - 2`), and like
// terms under such factors merge (`x + x` is `2 * x`). Validation subsequently
// inspects reducer placement, variable scope, and query-wide provenance without
// rewriting the term.
//
// ONE SPELLING PER MEANING. Two inputs that mean the same thing leave this pass
// spelled the same way, so no consumer has to know both. A reducer factor is always
// `factor * AGG` or `AGG / factor` (see PeelScale). `norm(e, p)` arrives from the
// binder as a SUM(e) aggregate whose alias names the order -- an aggregate whose
// meaning is not what it says -- and leaves as its definition: `SUM(ABS(e))` for
// p = 1, `SUM(POWER(e, 2))` for p = 2, `MAX(ABS(e))` for 'inf', each carrying a
// display-only WRITTEN_NORM_TAG_PREFIX tag so its text still reads as written. This
// replaces one atom with another and opens nothing. The L0 order needs indicator
// variables, which are formulation rather than meaning, so its marker stays for
// DecideOptimizer::RewriteNorm.
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
// by side (see context/docs/language/explain.md): with a pure
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
// loudly instead of being silently folded -- but it does fail. Any new
// optimizer pass that mutates a constraint in place must go through
// LogicalDecide::AddConstraint (which canonicalizes) rather than editing the
// tree directly.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/unordered_set.hpp"
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

//! One additive term of a canonical side, read back as canonicalization decomposed it.
struct CanonicalAtom {
	int sign;
	//! Borrowed from the side that was read.
	const Expression *term;
	//! True when canonicalization left a factor on a reducer; `scale` then names the reducer and the factor.
	bool scaled = false;
	ScaledAggregateMatch scale;
};

//! The additive terms of a canonical constraint side or objective body, without rebuilding anything. It descends
//! through `+`, `-` and binder casts over decision algebra by the same rule canonicalization decomposes with, so a
//! reader of canonical trees cannot disagree with the pass that built them.
vector<CanonicalAtom> ReadCanonicalAtoms(const Expression &side, idx_t decide_index);

//! The additive terms of a MIN or MAX body as canonicalization splits it: a constant addend moves beside the reducer
//! (`max(x + 0.5)` is `max(x) + 0.5`), every other term stays inside. The binder judges a strict bound on the terms
//! that stay, so the two cannot disagree on what moves.
struct ExtremumBody {
	vector<pair<int, const Expression *>> kept;
	vector<pair<int, const Expression *>> moved;
};
ExtremumBody SplitExtremumBody(const Expression &body, idx_t decide_index);

//! The semantic row shape of a canonical DECIDE comparison. VALUE is a comparison whose
//! decision side mixes a decision reducer with something the aggregate shape cannot hold
//! -- a per-row or keyed decision, a factor that varies by row, a second `by` key, a
//! reducer inside another expression: the value rewrite (stage 05) replaces each decision
//! reducer by a helper decision defined once per `by` group, after which the comparison
//! is PER_ROW. INVALID means the bound side bears a decision, or ClassifyScoped refused
//! the shape.
enum class CanonicalConstraintClass : uint8_t { PER_ROW, AGGREGATE, VALUE, INVALID };

//! What building the scope checks of the user's clauses reads besides the bound tree,
//! and the checks it builds (LogicalDecide::scope_checks), in clause order.
struct DecideScopeChecks {
	//! A check keyed by a scope's key and a `by` key together adds their union.
	vector<DecideKey> &keys;
	const vector<EntityScopeInfo> &entity_scopes;
	const vector<string> &source_fragments;
	vector<DecideScopeCheck> checks;
};

class DecideCanonicalizer {
public:
	//! Provenance-only form: no table-index evidence is available, so arbitrary column
	//! refs are never promoted to query-wide. Explicit QUERY_WIDE_VALUE_TAG metadata is
	//! still honored. Variable scopes remain available for homogeneity validation.
	//! Used by LogicalDecide::AddConstraint for optimizer output.
	//!
	//! `is_boolean[i]` says decision column `i` is declared BOOL (LogicalDecide::is_boolean_var), so a constraint may
	//! multiply a reducer by one: `b * SUM(x)` is a product the value rewrite hands to the Boolean linearization. It
	//! is required by every constructor: a canonicalizer built without it would refuse `b * SUM(x)` as a product of
	//! decisions, wherever it classifies a user's clause.
	DecideCanonicalizer(ClientContext &context, idx_t decide_index, vector<DecideVarScopeInfo> variable_scopes,
	                    const vector<bool> &is_boolean);

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
	DecideCanonicalizer(ClientContext &context, idx_t decide_index, vector<DecideVarScopeInfo> variable_scopes,
	                    const vector<bool> &is_boolean, unordered_set<idx_t> query_wide_table_indexes,
	                    unordered_set<idx_t> correlated_subquery_table_indexes = {});

	//! Canonicalize a whole SUCH THAT tree: AND-conjunctions plus the `when` and `per K:`
	//! wrappers. Descends only into the constraint child of a wrapper, never into its
	//! condition or key columns. Pure -- `constraints` is untouched.
	unique_ptr<Expression> CanonicalizeTree(const Expression &constraints) const;

	//! The scope checks of every clause of the user's SUCH THAT tree, appended to `scope`
	//! in clause order. Read-only, from the clauses as bound: before CanonicalizeTree,
	//! which drops the `per K:` of a clause in the aggregate shape -- one model row per
	//! `by` group already is one per class there -- while the checks are what that key
	//! still requires of the data. Called once, by the planner; a rewrite's clause
	//! (AddConstraint) never builds any.
	void BuildConstraintScopeChecks(const Expression &constraints, DecideScopeChecks &scope) const;

	//! The scope checks of the objective, built from
	//! the bound objective before CanonicalizeObjective: its `when` keeps a row, it is
	//! one value over the kept rows (`per ()`), and each reducer reads a row and is
	//! determined by its own `per`.
	void BuildObjectiveScopeChecks(const Expression &objective, DecideScopeChecks &scope) const;

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

	//! A reducer skips a row whose data it reads is NULL, as SQL's does (deck p. 50): for each
	//! reducer over decisions, every data value its body reads where a NULL makes the summand
	//! NULL is tested `IS NOT NULL` in the reducer's own `when`. A function that handles NULL
	//! itself (`COALESCE`) is not looked into. Runs on the canonical tree, before any rewrite
	//! copies a reducer's `when`, and after the clause's texts are recorded. A keyed decision
	//! the body reads has no value on a row whose written key column is NULL, so that column
	//! is tested too (`scopes` and `keys` name each decision's key).
	void SkipNullData(Expression &expr, const vector<EntityScopeInfo> &scopes, const vector<DecideKey> &keys) const;

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
	//! in release. Call sites call it unconditionally; in a release build only C0's scope
	//! rule (one scope per clause, one test per clause) runs. Build `debug` or `relassert`
	//! to exercise the rest.
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
		//! The term when Decompose built it rather than borrowed it (a MIN or MAX whose constant addends moved
		//! beside it); `expr` points into it.
		unique_ptr<Expression> owned;
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

	//! A MIN or MAX term with a constant addend in its body (SplitExtremumBody), pushed as the reducer over the rest
	//! and each constant beside it under the reducer's factor. False, pushing nothing, for any other term.
	bool DecomposeExtremumAddends(int sign, const Expression &term, const Expression &whole,
	                              unique_ptr<Expression> &scale, bool divides, vector<Atom> &out) const;

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
	//! (language/guide.md §6.4): it is peeled like any other and the comparison takes the
	//! VALUE shape, where the reducer becomes a helper decision the row's factor multiplies.
	const Expression &PeelScale(const Expression &expr, unique_ptr<Expression> &out_scale, bool &out_divides,
	                            Clause clause = Clause::CONSTRAINT) const;

	//! True when `expr` evaluates to a single value for the entire query, so it may
	//! scale a reducer. Constants qualify by folding; a column ref qualifies only by
	//! being listed in `query_wide_table_indexes`, because after flattening a
	//! `(SELECT max(w) FROM p)` is shape-identical to `weight` -- and, importantly, to
	//! a CORRELATED subquery, which is per-row and must not qualify.
	bool IsQueryWideConstant(const Expression &expr) const;

	//! True when `expr` is one decision declared BOOL, under the binder's casts, a sign, and factors known before the
	//! query runs: `b`, `-b`, `2 * b`. A reducer under such a factor takes the VALUE shape.
	bool IsBooleanFactor(const Expression &expr) const;
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
	void ValidateCanonicalTree(const Expression &constraints) const;
	void ValidateCanonicalComparison(const BoundComparisonExpression &comparison) const;
	void VerifyCanonicalTree(const Expression &constraints) const;
	//! A copy of `expr` with every norm marker of order 1, 2 or 'inf' replaced by its
	//! definition, or nullptr when there is none to replace.
	unique_ptr<Expression> DesugarNorms(const Expression &expr) const;
	void DesugarNormsInPlace(unique_ptr<Expression> &expr) const;
	unique_ptr<Expression> BindAggregate(const string &name, unique_ptr<Expression> child) const;
	//! `name(child)` carrying `from`'s alias and `when`: a reducer spelled anew keeps its tags and scope.
	unique_ptr<Expression> RebindReducer(const BoundAggregateExpression &from, const string &name,
	                                     unique_ptr<Expression> child) const;
	unique_ptr<Expression> CanonicalizeComparisonInternal(const Expression &comparison) const;
	unique_ptr<Expression> CanonicalizeObjectiveInternal(const Expression &objective,
	                                                     double &out_constant_offset) const;

	//! Wrapper-aware structural half of VerifyCanonicalObjective. Runs before the
	//! fixed-point check so a malformed body is reported as the invariant it breaks,
	//! rather than as whatever CanonicalizeObjective would have thrown re-reading it.
	void VerifyCanonicalObjectiveBody(const Expression &objective) const;
	void VerifyCanonicalComparison(const BoundComparisonExpression &comparison) const;

	unique_ptr<Expression> CanonicalizeTreeInternal(const Expression &constraints) const;

	//! The shape of a comparison's decision side (language/guide.md §6.4): ROWS has no
	//! decision reducer; AGGREGATE has decision reducers sharing one `by` key under
	//! whole-query factors with only query-wide terms beside them (one model row per group);
	//! VALUE needs the value rewrite; UNSUPPORTED is refused with `refusal`. `group_key` is
	//! the AGGREGATE shape's shared key, INVALID for `by ()`.
	enum class ScopedShape : uint8_t { ROWS, AGGREGATE, VALUE, UNSUPPORTED };
	ScopedShape ClassifyScoped(const BoundComparisonExpression &comparison, idx_t &group_key, string &refusal) const;
	//! Whether a comparison's decision side takes the composed MIN/MAX formulation
	//! (RewriteComposedMinMaxInConstraint): a MIN or MAX over decisions beside another
	//! term, or alone under a factor whose sign is not known before the query runs.
	bool TakesComposedMinMax(const vector<Atom> &atoms) const;

	//! The scope checks of one clause, `leaf` its
	//! comparison (or other constraint) as bound.
	void BuildClauseScopeChecks(const Expression &leaf, const DecideEnclosingScope &enclosing,
	                            DecideScopeChecks &scope) const;
	//! The checks of one reducer: a row in every `by` group (D8), and its own `per`
	//! determining its body. `rows` is the clause's `when`, null for every row.
	void BuildReducerScopeChecks(const BoundAggregateExpression &reducer, const Expression *rows,
	                             DecideScopeChecks &scope) const;

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
	//! Which decision columns are declared BOOL, indexed by decide-column index.
	vector<bool> boolean_decisions;

	//! `expr` as a scope refusal quotes it.
	string Quote(const Expression &expr) const;
	//! `expr` by its written text when the query recorded it, else by its bound name.
	string QuoteAsWritten(const Expression &expr) const;
};

} // namespace duckdb
