//===----------------------------------------------------------------------===//
//                         DeciDB
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
// THE CONTRACT. Every rule is an identity for every input. There are five:
//   - additive decomposition of each side;
//   - moving a term across the relation by whether it references a decision;
//   - distributing a factor known before the query runs over an additive spine
//     (`2 * (x - 1)` is `2 * x - 2`);
//   - merging like terms under such factors, over products as over atoms (`x + x` is
//     `2 * x`, `x*x + x*x` is `2 * POWER(x, 2)`);
//   - writing each product of decisions as a coefficient on the left times powers of
//     its decision factors (NormalizeProducts, decide_product_form.hpp).
// The pass never multiplies out a power or a product of sums, never folds constants,
// and never opens ABS, MIN/MAX or a subquery. It is total, pure, one spelling per
// meaning, and a fixed point. Validation of the algebra runs once, right after it
// (DecideShapeCheck), over that spelling.
//
// ONE SPELLING PER MEANING. Two inputs that mean the same thing leave this pass
// spelled the same way, so no consumer has to know both. A reducer factor is always
// `factor * AGG` or `AGG / factor` (see PeelScale). `norm(e, p)` arrives from the
// binder as a SUM(e) aggregate whose alias names the order -- an aggregate whose
// meaning is not what it says -- and leaves as its definition: `SUM(ABS(e))` for
// p = 1, `SUM(POWER(e, 2))` for p = 2, `MAX(ABS(e))` for 'inf', each keeping the
// order it was written with (BoundDecideReducerExpression::norm) so its text still reads as written. This
// replaces one atom with another and opens nothing. The L0 order needs indicator
// variables, which are formulation rather than meaning, so its marker stays for
// DecideOptimizer::RewriteNorm.
//
// What the contract leaves out is the whole design. A quadratic `POWER(x-t,2)`, a
// `MAX(x*v)`, and a filtered `sum(when c: x)` are decision-bearing terms
// here, moved and merged as `x` is; the product rule respells a product's factors
// and opens none of them. The symbolic layer this replaced had to carve out
// structural bypasses -- quadratic bodies, nested aggregates, opaque placeholders for
// ABS/MIN/MAX/subqueries -- precisely because it expanded terms to do its algebra. A
// pass that never expands a term has nothing to bypass, and can therefore be TOTAL.
// Totality is what lets this be the single home: a pass that may decline forces
// every consumer downstream to handle the declined case, which is precisely how
// canonicalization came to be spread across five sites.
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
//   2. LogicalDecide::AddClause     -- constraints the optimizer emits
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
// SHAPE IS DECIDED AT THIS BOUNDARY, once for a clause, and stored on it
// (BoundDecideClause::shape, group_key; ClassifyClause). After rebuilding, ClassifyScoped reads the
// decision side as one of three shapes: ROWS (no decision reducer: one row per kept
// row or class), AGGREGATE (decision reducers sharing one `by` key beside query-wide
// terms: one row per group) or VALUE (a decision reducer beside a per-row or keyed
// decision, a row-varying factor or a second `by` key: the value rewrite replaces each
// reducer by a helper decision). Only a decision on the bound side and the shapes
// ClassifyScoped names in its refusal are rejected.
//
// K1 is load-bearing: the physical operator has no partition logic of its own, and a
// check throws when a decision variable reaches the RHS. So a rewrite that breaks K1
// fails loudly instead of being silently folded. Any new
// optimizer pass that mutates a constraint in place must go through
// LogicalDecide::AddClause (which canonicalizes) rather than editing the
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
class BoundDecideReducerExpression;
class BoundComparisonExpression;
class BoundConjunctionExpression;
struct DecideSourceNames;
struct DecideProductForm;
class BoundColumnRefExpression;
class BoundFunctionExpression;

//! One-level reducer scale consumed after canonicalization. Exactly two spellings
//! reach any consumer -- `factor * AGG` and `AGG / factor` -- because BuildAdditive
//! re-attaches every peeled factor on the LEFT; objectives go through CanonicalizeObjective, so
//! `AGG * factor` (an unnormalized clause) never reaches a consumer.
struct ScaledAggregateMatch {
	const BoundDecideReducerExpression *aggregate = nullptr;
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

//! What building the variant signatures of the user's clauses writes (LogicalDecide::signatures).
struct DecideSignatures {
	//! A relation-qualified reducer over decisions whose body has a field gets a scope of its own, appended here
	//! (KeyedScopeInfo::signature), and its tag names it.
	vector<KeyedScopeInfo> &keyed_scopes;
	vector<DecideSignature> signatures;
};

//! What placing the frame selectors of the user's clauses writes (PlaceFrames): each frame's
//! place (DecideFrameSpec::rows, timeline_key, its D75 partition key), the keys its timelines
//! and partitions are split by appended to the key table when new.
struct DecideFramePlacement {
	vector<DecideFrameSpec> &frames;
	//! The key table, which placing appends the frames' timeline and partition keys to when new; the canonicalizer
	//! reads the same vector (its `keys`).
	vector<DecideKey> &keys;
	const vector<KeyedScopeInfo> &keyed_scopes;
	//! Which frames are placed: each stands in exactly one place.
	vector<bool> placed;
};

//! What a norm of `body` and order `order` stands for (language/guide.md §5): 1 `SUM(ABS(e))`, 2 `SUM(POWER(e, 2))`,
//! 'inf' `MAX(ABS(e))`, and 0 of data the count of its nonzero rows. Its type is the norm's: the binder gives it to the
//! marker, which parses as `SUM(e)`, so what reads the norm reads the value it holds.
unique_ptr<Expression> DecideNormDefinition(ClientContext &context, unique_ptr<Expression> body,
                                            const DecideNorm &norm);

class DecideCanonicalizer {
public:
	//! Whether `expr` holds a reducer anywhere, itself included. A frame's marker is none: `at(F: e)` is a point
	//! selection evaluated once per tuple (D80). A definition's body is none: the F node owns it, and its readers see
	//! the helper decision it defines.
	static bool ContainsReducer(const Expression &expr);
	//! Whether `expr` holds a reducer over a decision, as ContainsReducer reads it. A reducer over data alone is a
	//! value like any column, so `abs(x - max(t.v) by (t.g))` holds none.
	bool ContainsDecisionReducer(const Expression &expr) const;

	//! `is_boolean[i]` says decision column `i` is declared BOOL (LogicalDecide::is_boolean_var), so a constraint may
	//! multiply a reducer by one: `b * SUM(x)` is a product the value rewrite hands to the Boolean linearization. It
	//! is required: a canonicalizer built without it would refuse `b * SUM(x)` as a product of
	//! decisions, wherever it classifies a user's clause.
	//! `keyed_scopes` gives each keyed decision's key (KeyedScopeInfo::key), which decides whether a keyed decision
	//! beside a reducer is one value per group (ClassifyScoped). `keys` (LogicalDecide::keys) gives each key's
	//! elements, which decide whether a `per` key holds a value by construction (ScopeKeyOf, the signatures).
	//! `computed_values` (BoundDecide::computed_values) says which values are one for the whole query
	//! (IsQueryWideConstant). The test is FAIL-SAFE: anything not listed is treated as row-varying. The reverse
	//! framing (a deny-list of the relation's own bindings) reads the same for `weight` but silently admits a
	//! CORRELATED subquery, which also flattens onto a fresh table index while being per-row by construction.
	//! The canonicalizer reads the three vectors as they are when it asks, so entries added later are seen.
	DecideCanonicalizer(ClientContext &context, idx_t decide_index, vector<DecideVarScopeInfo> variable_scopes,
	                    const vector<KeyedScopeInfo> &keyed_scopes, const vector<DecideKey> &keys,
	                    const vector<bool> &is_boolean, const vector<DecideComputedValue> &computed_values);

	//! Canonicalize the clauses of SUCH THAT in place: each body, with its clause's scope; a `when` carries no
	//! algebra and is only rewritten to the one spelling. With `sources`, it reports in each clause's
	//! `canonical_differs` whether canonicalization changed that clause (its source clause names it).
	void CanonicalizeClauses(vector<BoundDecideClause> &clauses, vector<ConstraintSourceInfo> *sources = nullptr) const;

	//! Decide the shape of a canonical clause and the `by` key of its aggregate shape, with the clause's scope
	//! (BoundDecideClause::shape, group_key). CanonicalizeClauses calls it once for a clause; the one pass that
	//! replaces a clause's reducers by helpers (RewriteScopedReducers) calls it for the clause it leaves.
	void ClassifyClause(BoundDecideClause &clause) const;

	//! Canonicalize one row a function made after the clauses were canonicalized (an ABS envelope, an L0 link, a
	//! body definition row): a copy with the same spelling as a clause. Its shape is read off the row, aggregate
	//! where a decision reducer stands on its decision side: no scope decides it, and no value rewrite follows.
	BoundDecideClause CanonicalizeFunctionRow(const BoundDecideClause &row) const;

	//! The variant signature of every scoped expression of the canonical clauses with a field
	//! (language/guide.md §7.1; D70): each `per K:` clause, which names it (DecideClauseScope::signature), and
	//! each relation-qualified reducer over decisions, given a scope of its own (KeyedScopeInfo::signature). Its
	//! fields are the columns its tuple terms read (D83), its frames (D82) and decisions outside reducers, and the
	//! reducers' `by` keys; a field the key holds by construction is none. Called once, by the planner, right after
	//! CanonicalizeClauses.
	void BuildConstraintSignatures(vector<BoundDecideClause> &clauses, DecideSignatures &out) const;

	//! The objective's signature, its `scope.signature` -- it is produced once (D72), so execution refuses a field with
	//! a second value -- and its qualified reducers', after CanonicalizeObjective.
	void BuildObjectiveSignatures(BoundDecideClause &objective, DecideSignatures &out) const;

	//! The signature fields `expr` reads (D70, D82, D83), appended to `fields` unless there or held by `key` already: a
	//! decision by instance, a reducer's `by` key, a frame by its keys, and data by each column it reads.
	void CollectSignatureFields(const Expression &expr, idx_t key, vector<DecideSignatureField> &fields) const;

	//! Whether the classes of `per` key `key` that agree on every field `clause` reads are one instance. Not where it
	//! reads a field the key holds by construction (a column of the key, or a function of its columns), which the
	//! signature leaves out though its value differs between classes, or a frame, which reads another row.
	bool MergesClasses(const Expression &clause, idx_t key) const;

	//! Where every frame selector of the canonical clauses stands (language/guide.md §5.6): the rows its
	//! timelines read, which are its scoped expression's path rows (§5.5); the key they are grouped by, the path's
	//! `by` keys then `within`; the comparison it is written in; and whether it is inside a reducer
	//! (DecideFrameSpec::rows, timeline_key, source_clause_id, reduced). Records the partition check (D75) of each
	//! frame a `per` key reads directly -- a clause's `per K:`, or a reducer's own `per` -- unless the key determines
	//! its timeline already (DecideFrameSpec::partition_key). Refuses a reducer that may read no member under `abs`
	//! inside another reducer over decisions (D88). Called once, by the planner, after CanonicalizeClauses.
	void PlaceFrames(const vector<BoundDecideClause> &clauses, DecideFramePlacement &placement) const;
	//! The same for the objective, after CanonicalizeObjective. Its root has one class (D72), whose fields the
	//! objective's signature checks, so only a reducer's own `per` gets a partition check there.
	void PlaceObjectiveFrames(const BoundDecideClause &objective, DecideFramePlacement &placement) const;
	//! The frames a signature field may name (LogicalDecide::frames); null when the plan has none. Set by the
	//! planner before it builds the signatures.
	const vector<DecideFrameSpec> *frames = nullptr;

	//! Quote expressions in the scope refusals as the query writes them, from
	//! these names, rather than through the bound tree's own names. Set by the planner,
	//! which alone has them; `names` must outlive the canonicalizer.
	void QuoteMessagesWith(const DecideSourceNames &names) {
		message_names = &names;
	}

	//! True when `expr` evaluates to a single value for the entire query, so it may
	//! scale a reducer. Constants qualify by folding; a column ref qualifies only by
	//! being listed in `computed_values`, because after flattening a
	//! `(SELECT max(w) FROM p)` is shape-identical to `weight` -- and, importantly, to
	//! a CORRELATED subquery, which is per-row and must not qualify.
	bool IsQueryWideConstant(const Expression &expr) const;

	//! Canonicalize a bound DECIDE objective: its body; its `when` is kept. Pure -- `objective` is untouched.
	//!
	//! An objective is structurally ONE SIDE of a comparison. There is no relation to
	//! orient and no bound to separate, so the two side-partitioning rules (decisions
	//! left, data right) have no analogue here. Everything else this boundary owns
	//! applies unchanged -- additive decomposition, decision-aware
	//! cast transparency, and one spelling for a reducer scale -- and is SHARED with
	//! CanonicalizeClauses rather than reimplemented, which is the whole reason
	//! objectives get a boundary instead of their own normalizer.
	//!
	//! Additive decision-free terms are folded into `out_constant_offset`, which is
	//! ADDED to rather than assigned, so re-canonicalizing through
	//! LogicalDecide::SetObjective accumulates correctly. Such a term shifts the
	//! objective without moving its argmax/argmin, but must be added back to report an
	//! objective VALUE.
	//!
	//! An objective with no decision content at all is returned unchanged.
	//!
	//! The result is always a minimization. `sense` is the direction the objective was written in: MAXIMIZE writes
	//! `f` as `-f`, the flip carried by the sign of each term, never by a factor, and it is the last step (the
	//! objective is canonicalized as written, so like terms merge as in a MINIMIZE). Every stage after this one reads a
	//! minimization; what the user wrote is LogicalDecide::written_sense, which only messages, EXPLAIN and a shown
	//! value read. An objective an optimizer rewrite hands back (LogicalDecide::SetObjective) is a minimization
	//! already.
	BoundDecideClause CanonicalizeObjective(const BoundDecideClause &objective, double &out_constant_offset,
	                                        DecideSense sense = DecideSense::MINIMIZE) const;

	//! A reducer skips a row whose data it reads is NULL, as SQL's does (deck p. 50): for each
	//! reducer over decisions, every data value its body reads where a NULL makes the summand
	//! NULL is tested `IS NOT NULL` in the reducer's own `when`. A function that handles NULL
	//! itself (`COALESCE`) is not looked into. Runs on the canonical tree, before any rewrite
	//! copies a reducer's `when`, and after the clause's texts are recorded. A keyed decision
	//! the body reads has no value on a row whose written key column is NULL, so that column
	//! is tested too (`scopes` and `keys` name each decision's key).
	void SkipNullData(Expression &expr, const vector<KeyedScopeInfo> &scopes, const vector<DecideKey> &keys) const;

	//! Assert the structural invariant owned by this boundary. This is deliberately
	//! non-mutating and throws InternalException: once binding/canonicalization has
	//! accepted a constraint, a non-canonical clause is an engine bug rather than a
	//! user-input error.
	//!
	//! DEBUG-ONLY, like LogicalOperator::Verify: the body re-canonicalizes the
	//! clause to check idempotency, which is too expensive to pay on every DECIDE query
	//! in release. Call sites call it unconditionally. Build `debug` or `relassert`
	//! to exercise it.
	void VerifyCanonical(const BoundDecideClause &clause) const;

	//! The objective counterpart of VerifyCanonical, against the canonical form
	//! CanonicalizeObjective gives an objective. Non-mutating, and throws InternalException for the same reason:
	//! past this boundary a non-canonical objective is an engine bug, not user input.
	//!
	//! DEBUG-ONLY -- see VerifyCanonical.
	void VerifyCanonicalObjective(const BoundDecideClause &objective) const;

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
	//! A data-only reducer is RIGHT: the right-hand side evaluates any DuckDB aggregate over its
	//! groups, so K1 reads decisions left, data right, with no third case.
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
		//! Where the user wrote what the atom is the value of, set where it is that value alone (no factor distributed
		//! onto it, no sign peeled off it): the rebuilt `scale * expr` is equal to it, so it stands there
		//! (DecideReplaces). Like atoms merged into one stand where the least expression holding them all does.
		optional_idx written;
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
	//! The term `atom` is a multiple of -- its product form without the constant -- and that multiple as a number
	//! known before the query runs: its sign, the factor PeelScale peeled off a reducer (a divisor its reciprocal),
	//! its constant divisors' reciprocals and the product's constant coefficient. False when a factor is data or a
	//! subquery, which is not summable here.
	bool ReadLikeTermMultiple(const Atom &atom, DecideProductForm &term, double &multiple) const;
	//! Make `atom` the merged group: `multiple` times `term`.
	void ScaleAtomBy(Atom &atom, const DecideProductForm &term, double multiple) const;

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
	//!
	//! A minus over a factor that reads a decision (`-b * SUM(x)`) is the term's sign, not part of the factor: it is
	//! taken off the factor and `out_negated` says the term is negated. A minus over data stays where it is, since
	//! DuckDB evaluates it and can raise.
	const Expression &PeelScale(const Expression &expr, unique_ptr<Expression> &out_scale, bool &out_divides,
	                            bool &out_negated, Clause clause = Clause::CONSTRAINT) const;
	//! A copy of `factor` without the minus signs over what it reads of a decision (`-b`, `-(2 * b)`, `2 * -b`), with
	//! `negated` toggled for each.
	unique_ptr<Expression> WithoutDecisionNegation(const Expression &factor, bool &negated) const;

	//! True when `expr` is one decision declared BOOL, under the binder's casts, and factors known before the query
	//! runs: `b`, `2 * b`. A reducer under such a factor takes the VALUE shape.
	bool IsBooleanFactor(const Expression &expr) const;
	//! The scope of a tuple term, as a key of the key lattice (JoinDecideKeys): the join of its factors' scopes. A
	//! decision has its own key (decision_keys); data `data_key` -- the enclosing `per` key -- where that key holds it
	//! by construction (one of its elements), else DECIDE_ROW_KEY, since a value the key does not hold may take several
	//! values in a class (D70); and a value known for the whole query the empty key.
	idx_t ScopeKeyOf(const Expression &expr, idx_t data_key) const;

	//! Whether `body` is a comparison whose bound is one value for the whole query: every component of the complete
	//! canonical bound is query-wide (BoundDecideClause::query_wide_bound). Read from the bound each time a clause
	//! is canonicalized, since a rewrite may have replaced its children.
	bool BoundIsQueryWide(const Expression &body) const;

	//! The refusal of a decision-bearing `factor` on a reducer: the decision's type when `positive` (the factor without
	//! its minus signs) is one decision, else that it reads one. `divides` says it divides the reducer.
	string DecisionScalesReducerMessage(const Expression &factor, const Expression &positive, bool divides) const;

	//! The one question this pass asks of a term. Never inspects further.
	Placement Classify(const Expression &expr) const;

	bool ReferencesDecideVar(const Expression &expr) const;
	//! `sources` (null for none) gives a clause's span, which an error points at where the comparison has none.
	void ValidateCanonicalComparison(const BoundComparisonExpression &comparison, idx_t source_clause_id,
	                                 const vector<ConstraintSourceInfo> *sources) const;
	//! A copy of `expr` with every norm marker of order 1, 2 or 'inf' replaced by its
	//! definition, or nullptr when there is none to replace.
	unique_ptr<Expression> DesugarNorms(const Expression &expr) const;
	void DesugarNormsInPlace(unique_ptr<Expression> &expr) const;
	unique_ptr<Expression> BindAggregate(DecideReducerKind kind, unique_ptr<Expression> child) const;
	//! `from`'s kind over `child`, carrying its alias, facts and `when`.
	unique_ptr<Expression> RebindReducer(const BoundDecideReducerExpression &from, unique_ptr<Expression> child) const;
	unique_ptr<Expression> CanonicalizeComparisonInternal(const Expression &comparison) const;
	//! `value_shape`: a reducer stands under a Boolean decision, so the value rewrite holds it in a helper decision.
	unique_ptr<Expression> CanonicalizeObjectiveInternal(const Expression &objective, double &out_constant_offset,
	                                                     bool negate, bool &value_shape) const;

	void VerifyCanonicalComparison(const BoundComparisonExpression &comparison) const;

	//! The canonical form of a clause's body: a comparison canonicalized, any other body (an IN, a placeholder) as
	//! it is.
	unique_ptr<Expression> CanonicalizeClauseBody(const Expression &body) const;

	//! The shape of a comparison's decision side (language/guide.md §6.4): ROWS has no
	//! decision reducer; AGGREGATE has decision reducers sharing one `by` key under
	//! whole-query factors with only query-wide terms beside them (one model row per group);
	//! VALUE needs the value rewrite; UNSUPPORTED is refused (an AVG in a `<>` clause beside another term).
	//! `group_key` is the AGGREGATE shape's shared key, INVALID for `by ()`.
	enum class ScopedShape : uint8_t { ROWS, AGGREGATE, VALUE, UNSUPPORTED };
	//! `data_key` is the scope of the clause's data (ScopeKeyOf).
	ScopedShape ClassifyScoped(const BoundComparisonExpression &comparison, idx_t data_key, idx_t &group_key) const;

	//! Whether the key `key` holds `field` by construction: one class shows one value of it.
	bool KeyHoldsField(idx_t key, const DecideSignatureField &field) const;
	//! Gives every relation-qualified reducer over decisions in `expr` whose body has a field its signature and a
	//! scope of its own, counted over the rows its path keeps (language/guide.md §5.5).
	void SignQualifiedReducers(Expression &expr, const DecideReducerPath &path, Clause clause, idx_t source_clause_id,
	                           DecideSignatures &out) const;

	//! The `per` key a scoped expression is read under, for its frames' partition check (D75):
	//! its key and the key as written.
	struct FramePerKey {
		//! Into `keys`; INVALID_INDEX for `per ()`, one class.
		idx_t key = DConstants::INVALID_INDEX;
		//! The scope as a message names it: `per D.depotID`, `the objective (per ())`.
		string text;
	};
	//! The frames of `expr` along `path`: one outside every reducer of its comparison
	//! stands at the comparison's root, one in a reducer's argument reads that reducer's
	//! members. `per` is the key of the scoped expression `expr` is the body of; null for
	//! `per row`, which no frame's timeline can span.
	void PlaceFramesIn(const Expression &expr, const DecideReducerPath &path, const FramePerKey *per,
	                   DecideFramePlacement &placement) const;
	//! The key of a reducer's own `per`, when it has one.
	bool ReducerPerKey(const BoundDecideReducerExpression &reducer, const vector<KeyedScopeInfo> &keyed_scopes,
	                   FramePerKey &out) const;

	//! Rebuild an additive chain from terms.
	unique_ptr<Expression> BuildAdditive(const vector<Atom> &atoms) const;
	//! Whether `side` spells one term with signs around it (`-(-max(e))`): its canonical
	//! form is the bare term, which only a rebuild gives.
	bool IsSignedSpine(const Expression &side, const vector<Atom> &atoms) const;

	//! `written_name` is what a plan prints for a function with aliases (`power` for `**`), empty for its primary.
	unique_ptr<Expression> BindOp(const string &name, unique_ptr<Expression> left, unique_ptr<Expression> right,
	                              const string &written_name = string()) const;
	//! Unary form, for the leading negative term of a rebuilt spine.
	unique_ptr<Expression> BindOp(const string &name, unique_ptr<Expression> operand,
	                              const string &written_name = string()) const;

	ClientContext &context;
	idx_t decide_index;
	//! Scope assignment indexed by decide-column index. Missing entries fail safe
	//! as row-scoped.
	vector<DecideVarScopeInfo> variable_scopes;
	//! The key of each decision column's scope (JoinDecideKeys): the empty key for a query-wide decision, its key for
	//! a keyed one, DECIDE_ROW_KEY for one per row. Missing entries are per row.
	vector<idx_t> decision_keys;
	//! See QuoteMessagesWith; null quotes through the bound tree (UserFacingName).
	optional_ptr<const DecideSourceNames> message_names;
	//! Which decision columns are declared BOOL, indexed by decide-column index.
	vector<bool> boolean_decisions;
	//! The keyed scopes and keys (LogicalDecide::keyed_scopes, LogicalDecide::keys), read where they are.
	const vector<KeyedScopeInfo> &keyed_scopes;
	const vector<DecideKey> &keys;
	//! The computed values read whole (BoundDecide::computed_values), read where they are.
	const vector<DecideComputedValue> &computed_values;

	//! `expr` as a scope refusal quotes it: the text the user wrote for it, parenthesized where it binds looser than
	//! the operator it is an operand of (`parent_precedence`, as in RenderDecideSource).
	string Quote(const Expression &expr, int parent_precedence = 0) const;
};

} // namespace duckdb
