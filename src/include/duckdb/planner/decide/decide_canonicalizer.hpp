//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/decide/decide_canonicalizer.hpp
//
// The single home for DECIDE constraint canonicalization.
//===----------------------------------------------------------------------===//
//
// CANONICAL FORM
// --------------
// A bound DECIDE constraint is canonical when it is a BoundComparisonExpression
// `L <op> R` where R contains no decision-variable reference, and every
// decision-bearing term lives in L. Canonicalization decides that shape once;
// nothing downstream re-decides it.
//
// THE RULE THAT MAKES THIS WORK: this pass NEVER looks inside a term. It
// decomposes each side additively and asks one question of each resulting term
// -- "does it reference a decision variable?" -- then moves terms across the
// relation accordingly. It performs no algebra: it does not expand, combine
// like terms, fold constants, or distribute.
//
// That restriction is the whole design. A quadratic `POWER(x-t,2)`, a composed
// `MAX(x*v)`, and a filtered `SUM(x) WHEN c` are all simply decision-bearing
// terms here, indistinguishable from `x`. The three structural bypasses in the
// symbolic layer exist because that layer must open terms to do its algebra;
// a pass that never opens a term has nothing to bypass, and can therefore be
// TOTAL. Totality is what lets this be the single home: a pass that may decline
// forces every consumer downstream to handle the declined case, which is
// precisely how canonicalization came to be spread across five sites.
//
// PURITY: Canonicalize* take their input by const reference and return a new
// tree, leaving the input untouched. This is deliberate and load-bearing for a
// planned EXPLAIN feature that renders as-written / canonical / rewritten side
// by side (see context/descriptions/01_pipeline/04_explain.md): with a pure
// function both forms are live at the call site, so that feature is a rendering
// change rather than a re-plumbing job.
//
// CALL SITES (exactly two, and there must never be a third):
//   1. Binder::PlanSelectNode  -- everything the user wrote
//   2. LogicalDecide::AddConstraint -- everything the optimizer emits
//
// CASTS ARE PART OF THE SPINE, NOT A LID ON IT. A bound tree is typed, and
// FunctionBinder::CastToFunctionArguments inserts a cast on any scalar
// function's children whose types do not match the resolved signature. So casts
// appear *between* the `+` nodes of an additive chain, not merely wrapping the
// side (bind_comparison_expression.cpp wraps each whole side too, when the two
// sides' types differ). This pass emits them as well as consuming them: BindOp
// rebuilds `+`/`-` through the same FunctionBinder. Descending widening numeric
// casts is therefore what makes the pass a fixed point on its own output; without
// it, `Canon(Canon(c)) != Canon(c)`. Narrowing casts stay term boundaries, since
// `CAST(1.6 + 1.6 AS INT)` is 3 and `CAST(1.6 AS INT) + CAST(1.6 AS INT)` is 4.
//
// SCALE IS PART OF THE ATOM, NOT OF THE TREE SHAPE. The additive spine peels a
// query-wide factor outward off a decision-bearing reducer, so a term is
// {sign, scale, expr} and every consumer reads one spelling: `scale * term`, with
// the scale on the left (or `term / scale` for a division, which is left as a
// division rather than reciprocated). This replaced a parsed-level fold that
// pushed the factor INWARD, which was unsound for MIN/MAX under a negative factor.
// Peeling is also where the "a factor on a reducer must be one value for the whole
// query" rule is enforced -- the one rejection this pass currently owns.
//
// STAGING NOTE: this pass restructures only what must move and returns an
// unchanged copy otherwise -- notably it does not yet implement the full
// classification-driven rejection (K3).
//
// K1 is now load-bearing rather than advisory. The physical operator's duplicate
// partition logic (its per-row branch and lhs_offset_expr) was deleted at
// canonicalize.md C.3 after being verified unreachable across the golden corpus
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

class DecideCanonicalizer {
public:
	//! Permissive form: no information about which column refs are query-wide, so the
	//! pass does not judge a factor's row-varying-ness at all (every column ref counts
	//! as query-wide). Used by LogicalDecide::AddConstraint, which canonicalizes what
	//! the optimizer emits -- those trees are synthesized, not user-written, and the
	//! subquery-flattening evidence this judgement needs no longer exists by then.
	DecideCanonicalizer(ClientContext &context, idx_t decide_index);

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
	                    unordered_set<idx_t> query_wide_table_indexes,
	                    unordered_set<idx_t> correlated_subquery_table_indexes = {});

	//! Canonicalize a whole SUCH THAT tree: AND-conjunctions plus WHEN/PER
	//! wrappers. Descends only into the constraint child of a wrapper, never
	//! into its condition or PER columns. Pure -- `constraints` is untouched.
	unique_ptr<Expression> CanonicalizeTree(const Expression &constraints) const;

	//! Canonicalize a single comparison. Pure -- `comparison` is untouched.
	unique_ptr<Expression> CanonicalizeComparison(const Expression &comparison) const;

private:
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
	//! tree and is only ever copied, never mutated. `cast_type` is the type of a
	//! widening cast the spine descended through on the way to this term; the
	//! rebuild re-applies it per term, since that is the only way a cast that
	//! wrapped a whole side can survive the side being split.
	//!
	//! `scale` is a factor peeled off a reducer-bearing term: `2 * SUM(x)` becomes
	//! {scale=2, expr=SUM(x)} rather than one opaque term. Peeling moves the factor
	//! OUTWARD and is the opposite of the fold it replaces -- folding inward
	//! (`2 * MAX(x)` -> `MAX(2*x)`) is unsound for MIN/MAX under a negative factor,
	//! since `MAX(-2x)` is `-2*MIN(x)`. Crucially, peeling does not open the term:
	//! this pass still cannot tell `SUM(x)` from `POWER(x-t,2)`, so totality survives.
	struct Atom {
		int sign;
		const Expression *expr;
		Placement placement;
		const LogicalType *cast_type;
		//! Factor peeled off `expr`, or nullptr. Borrowed from the input tree.
		const Expression *scale = nullptr;
		//! false: the term was `scale * expr`. true: it was `expr / scale`.
		//! Kept as division rather than reciprocated -- `SUM(x)/3` has no exact
		//! decimal reciprocal, and the consumers divide as happily as they multiply.
		bool scale_divides = false;
	};

	//! Split an expression into signed additive terms. Descends through `+`,
	//! binary `-`, unary `-`, and widening numeric casts; every other node is a
	//! term boundary. `cast_type` is the pending re-cast for terms found below a
	//! cast the walk descended through, or nullptr at the top of a side.
	void Decompose(const Expression &expr, int sign, vector<Atom> &out, const LogicalType *cast_type) const;

	//! If `expr` is a decision-bearing reducer with a factor on it, split the factor
	//! out into `out_scale` / `out_divides` and return the bare reducer term. Returns
	//! `expr` unchanged when there is nothing to peel.
	//!
	//! Throws when the factor is not a legal scale, which is the single place that
	//! judgement is made. A factor multiplying a reducer must be ONE value for the
	//! whole query: `weight * SUM(x)` asks which row's weight scales a number that has
	//! no row, and `s * SUM(x)` is a product of two decisions (bilinear), not a scale.
	const Expression &PeelScale(const Expression &expr, const Expression *&out_scale, bool &out_divides) const;

	//! True when `expr` evaluates to a single value for the entire query, so it may
	//! scale a reducer. Constants qualify by folding; a column ref qualifies only by
	//! being listed in `query_wide_table_indexes`, because after flattening a
	//! `(SELECT max(w) FROM p)` is shape-identical to `weight` -- and, importantly, to
	//! a CORRELATED subquery, which is per-row and must not qualify.
	bool IsQueryWideConstant(const Expression &expr) const;

	//! True when `expr` is what a CORRELATED scalar subquery flattened into. Affects
	//! only the wording of a rejection, never the decision.
	bool IsCorrelatedSubqueryRef(const Expression &expr) const;

	//! True when `to` can represent every value of `from` exactly, so that
	//! `CAST(a + b AS to)` and `CAST(a AS to) + CAST(b AS to)` agree. Only such a
	//! cast may be pushed onto the individual terms of an additive spine.
	static bool IsWideningNumericCast(const LogicalType &from, const LogicalType &to);

	//! The one question this pass asks of a term. Never inspects further.
	Placement Classify(const Expression &expr) const;

	bool ReferencesDecideVar(const Expression &expr) const;
	static bool ContainsReducer(const Expression &expr);

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
	//! Table indexes that uncorrelated scalar subqueries flattened into. Meaningful
	//! only when `judge_column_refs` is set.
	unordered_set<idx_t> query_wide_table_indexes;
	//! Table indexes that CORRELATED scalar subqueries flattened into. Never affects
	//! the decision (they are row-varying by construction, and would be rejected by the
	//! fail-safe default anyway) -- only the wording of the rejection.
	unordered_set<idx_t> correlated_subquery_table_indexes;
	//! False when the pass was given no flattening evidence, in which case it declines
	//! to classify a column ref as row-varying rather than guessing.
	bool judge_column_refs = false;
};

} // namespace duckdb
