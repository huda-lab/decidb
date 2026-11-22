//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/optimizer/decide/decide_optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_set.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

//! The ABS nodes whose auxiliary needs the exact (Big-M) encoding.
using AbsExactSet = unordered_set<const Expression *>;

class DecideCanonicalizer;
class LogicalDecide;
class Optimizer;

//! The DecideOptimizer performs algebraic rewrites on LogicalDecide nodes.
//! This centralizes all DECIDE-specific transformations that were previously
//! scattered across the binder and physical operator.
//!
//! OptimizeDecide runs, per DECIDE node:
//!   - RewriteRhsAggregates (pass 0, every path): the bound-side evaluator, whose output the rules and the solver read.
//!     It computes nothing: the reducers a bound reads are recorded, and the path builds their side tables
//!     (DirectSourceReader::AddSideTable)
//!   - ChooseDecideAnswer (decide_answer_choice.hpp): a direct-solve rule replaces the node, else a solver is chosen
//! and, when a solver is, the formulation passes of RewriteForSolver, in this order:
//!   - RewriteNorm: lowers the bound NORM marker, including L0 indicators/links
//!   - RewriteInDomain: lowers DECIDE variable IN lists to indicator formulations
//!   - RewriteBilinear: McCormick linearization for Boolean × anything products
//!   - RewriteMinMax: the easy identity of a MIN/MAX constraint (`max(e) <= K` holds when every member does), and
//!     an equality's split into its two relations. Runs before the passes that read a clause's direction
//!   - RewriteScopedReducers: each decision reducer of a VALUE-shape comparison, and every other MIN/MAX in a
//!     constraint or the objective, becomes a helper decision defined per group
//!   - RewriteNotEqual: creates indicator variables for <> constraints
//!   - RewriteAvgToSum: rewrites AVG(expr) → SUM(expr) with alias tag for RHS scaling
//!   - RewriteAbs: detects ABS(expr) over decide vars, creates auxiliary REAL vars, retains the written function with
//!     its linearization constraints, and decides each ABS's encoding once, from the push its owner gives it, after
//!     every rewrite that joins a helper's readers has run
//!   - FoldSingletonRows: folds simple `x OP const` bounds into the column box.
//!     Must run LAST — see its declaration.
//!
class DecideOptimizer {
public:
	explicit DecideOptimizer(Optimizer &optimizer);

	//! Recursively optimize the plan, transforming any LogicalDecide nodes found
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> plan);

	//! Throws InternalException when a clause no longer has what bound reduction relied on
	//! (BoundDecideClause::bound_read_shape, bound_read_type): one read in another shape is in the aggregate shape, or
	//! one whose bound is its group's tightest value has another direction. A DEBUG build calls it after every pass.
	static void VerifyBoundReads(const LogicalDecide &decide);

private:
	//! Optimize the subtree held by `op`, which sits inside the plan held by `root`. A direct-solve rule that replaces
	//! a DECIDE node rewrites the references above it from `root`.
	void OptimizeSubtree(unique_ptr<LogicalOperator> &op, unique_ptr<LogicalOperator> &root);

	//! The bound-side evaluator: replaces each reducer on a bound by a placeholder column that stands for the same
	//! aggregate over its groups (LogicalDecide::bound_aggregates), reduces the bound of an aggregate-shape clause to
	//! its group's tightest value, and appends the VALUE_IS_VALID checks of what those windows read. The solver and the
	//! direct-solve rules then read one number that DuckDB computed. Runs first, in every mode, because both paths must
	//! see it.
	void RewriteRhsAggregates(LogicalDecide &decide);

	//! Optimize the DECIDE node held by `op`, which sits inside the plan held by `root`: pass 0, the choice of how it
	//! is answered, and for the solver the formulation passes. A rule that answers the query replaces `op`. The whole
	//! stage is one profile span, whichever way the query is answered.
	void OptimizeDecide(unique_ptr<LogicalOperator> &op, unique_ptr<LogicalOperator> &root);
	//! The passes that formulate the query for the solver chosen, in order.
	void RewriteForSolver(LogicalDecide &decide, LogicalOperator &root);
	//! Before any one-to-many rewrite, assign written-order identities to source clauses
	//! whose formulations may only be removed as an atomic diagnostic repair.
	void TagAtomicRemovalGroups(LogicalDecide &decide);

	//! Lower NORM markers before ABS/MINMAX sees their generated expressions.
	void RewriteNorm(LogicalDecide &decide);
	//! Lower bound COMPARE_IN markers into the existing exact indicator formulation.
	void RewriteInDomain(LogicalDecide &decide);

	//! Rewrite not-equal (<>) constraints: refuse one over a product of decisions, and account for each in the plan.
	//! The disjunction, its indicator binaries and its Big-M are generated at execution time when bounds are known.
	void RewriteNotEqual(LogicalDecide &decide);

	//! Fold every simple `x OP const` constraint (a BETWEEN is two of them by now) into the
	//! decision column's box (`singleton_row_boxes`, combined by `DecideFoldSingletonRow`) rather
	//! than leaving it to become a model row: a box is smaller and tighter for the solver than
	//! one row per data row. Folded comparisons are marked (BoundDecideClause::folded_bound) so physical
	//! term extraction skips them; a recorded one (`user_singleton_row_bounds`) also keeps its
	//! clause rows (folded_clause_rows), so its clause is flattened for the repair model.
	//!
	//! MUST RUN LAST, after every other pass. RewriteInDomain emits a floor-lowering
	//! `x >= min_value` for a negative all-constant IN domain, which is itself a singleton-row bound;
	//! run earlier and that floor stays at 0 and the negative domain value silently
	//! becomes unreachable. Running last also guarantees every auxiliary variable already
	//! exists, so `is_boolean_var` is complete.
	//!
	//! Declines rather than throws. A strict `<` / `>` on a REAL variable has no valid
	//! folding, so it is deliberately left as a comparison for the model builder to
	//! reject with a user-facing message.
	void FoldSingletonRows(LogicalDecide &decide);

	//! Helper: fold the bound one clause states, if it states one. A clause under a `when`, or under a `per K:`
	//! whose key has a NULL-skipping column, never contributes to a global bound.
	void FoldSingletonRowsInClause(BoundDecideClause &clause, LogicalDecide &decide);

	//! Rewrite decision-bearing AVG(expr) aggregates to SUM(expr) with alias tagging.
	//! Execution scales extracted AVG terms by the active row count.
	void RewriteAvgToSum(LogicalDecide &decide);

	//! Helper: recursively walk a bound expression tree, replacing AVG aggregates with SUM.
	//! The replacement says so (BoundDecideReducerExpression::count_scale) so coefficient evaluation can scale terms.
	//! A decision-free AVG is skipped: there is nothing to linearize, and rebinding it as SUM
	//! would redeclare it with SUM's integral type while its value stays fractional.
	//! `origin` is the clause the walk stands in (the objective's origin for the
	//! objective), under which each rewritten AVG's account is recorded.
	//! Whether it rewrote an AVG under `expr`.
	bool RewriteAvgInExpression(unique_ptr<Expression> &expr, LogicalDecide &decide, const DecideRowOrigin &origin);

	//! The value rewrite (language/guide.md §6.4): a decision reducer of a comparison in
	//! the VALUE shape (BoundDecideClause::shape) becomes a helper decision defined once per `by`
	//! group (LogicalDecide::reducer_value_links), and the comparison is then one row per
	//! class. A direct, unfiltered outer SUM with the same key as its clause stays in the
	//! tree; its nested reducers still become helpers, innermost first, keyed by their path
	//! (language/guide.md §5.5). Objective reducers tagged for holding one become helpers,
	//! except the group-sum rewrite (D81). Runs after ABS and bilinear, whose auxiliaries may
	//! stand inside a reducer's body, and before the MIN/MAX passes, which never see these
	//! reducers. A product of decisions left inside such a reducer is refused
	//! (language/errors.md §2).
	void RewriteScopedReducers(LogicalDecide &decide);

	//! The objective's `sum(inner(e) by (G))`, the outer sum with no `per` and `e` not linear, summed in the other
	//! order (deck p. 51; owner, 2026-11-05): one sum of `e`, each row weighted by the rows of its G group the outer
	//! reads -- a reducer over data, `sum(1) by (G)`, read on the outer's path -- divided, for an AVG, by the group's
	//! own members, and 0 where the inner `when` drops the row. A square of a linear form takes the weight inside, by
	//! its root, so `MINIMIZE sum(sum(x * x) by (G))` is a convex QP rather than a helper with a quadratic
	//! definition. Runs first, so the bound-side rewrite computes the weights.
	void SwapObjectiveSummation(LogicalDecide &decide);

	//! Range frames over decisions (language/guide.md §5.6, D78): each becomes a helper decision with one instance per
	//! window of the frame -- a timeline, or a position on one -- defined once per window
	//! (LogicalDecide::reducer_value_links, with its frame), so every later rewrite reads it as an ordinary keyed
	//! decision and the canonical shape stands. Runs first among the rewrites, before any copies a clause; a MIN/MAX
	//! helper is pinned on the side its comparison pushes it, through SUM, AVG, MIN and MAX (D79).
	void RewriteFrameRanges(LogicalDecide &decide);

	//! The easy identity of a MIN or MAX in a constraint: a lone `max(e) <= K` or `min(e) >= K` keeps its reducer,
	//! tagged, and bounds each member; an equality is split into that and the other relation, which the helper takes.
	//! The other MIN/MAX clauses are RewriteScopedReducers'.
	void RewriteMinMax(LogicalDecide &decide);

	//! Helper: the easy identity of one clause. A row this rewrite adds for the clause (an equality's easy half, in
	//! `new_clauses`) keeps its `when` and `per K:`.
	void RewriteMinMaxInClause(BoundDecideClause &clause, LogicalDecide &decide,
	                           vector<BoundDecideClause> &new_clauses);

	//! Detect ABS(expr) over decide variables, allocate the plan-time helper, and record each ABS
	//! (LogicalDecide::abs_functions) for the tree's ABS F node: the shape only, which stage 06 encodes.
	void RewriteAbs(LogicalDecide &decide);

	//! Annotate bilinear products (x * y) where at least one factor is Boolean, retaining the written expression for
	//! the tree and the shared term splitter while allocating helper columns. Boolean × Boolean: w = b1 * b2,
	//! AND-linearization (w <= b1, w <= b2, w >= b1+b2-1). Boolean × Other: w = b * x, partial McCormick (w <= x at
	//! plan time,
	//!   w <= U*b and w >= x-U*(1-b) at execution time via BilinearLinkSpec).
	//! Non-Boolean × Non-Boolean: left in place for Q matrix handling in physical operator.
	void RewriteBilinear(LogicalDecide &decide);

	//! Helper: find affine bilinear products, collect their distributive expansion and product F specs, and leave the
	//! source expression intact.
	//! `origin` is the origin of the clause the expression stands in, so the rows this
	//! emits can point back at the clause that produced them.
	void FindAndReplaceBilinear(unique_ptr<Expression> &expr, LogicalDecide &decide, const DecideRowOrigin &origin);

	//! Helper: recursively find BoundFunctionExpression for ABS over decide vars, allocate a helper, tag the retained
	//! function expression and record the ABS (LogicalDecide::abs_functions), innermost first. Whether the ABS needs
	//! its exact encoding is read from `exact`, which RewriteAbs filled from every body's owner; this walk decides
	//! nothing about it. `rows` is the rows the expression is read on (DecideOwnedBody::rows, null for every row),
	//! which the `when` of each reducer around an ABS narrows further.
	void FindAbsAndAllocateAux(unique_ptr<Expression> &expr, LogicalDecide &decide,
	                           const DecideCanonicalizer &canonicalizer, const AbsExactSet &exact,
	                           const DecideRowOrigin &origin, const Expression *rows);

	Optimizer &optimizer;
};

} // namespace duckdb
