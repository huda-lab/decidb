//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/optimizer/decide/decide_optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

class DecideCanonicalizer;
class LogicalDecide;
class Optimizer;

//! The DecideOptimizer performs algebraic rewrites on LogicalDecide nodes.
//! This centralizes all DECIDE-specific transformations that were previously
//! scattered across the binder and physical operator.
//!
//! OptimizeDecide runs, per DECIDE node:
//!   - RewriteRhsAggregates (pass 0, every path): the bound-side evaluator, whose output the rules and the solver read
//!   - ChooseDecideAnswer (decide_answer_choice.hpp): a direct-solve rule replaces the node, else a solver is chosen
//! and, when a solver is, the formulation passes of RewriteForSolver, in this order:
//!   - InstallReducedBounds: the windows of the bounds pass 0 left to a rule (DecideGroupBound)
//!   - RewriteNorm: lowers the bound NORM marker, including L0 indicators/links
//!   - RewriteInDomain: lowers DECIDE variable IN lists to indicator formulations
//!   - TagAbsForBigM: marks the ABS nodes needing a Big-M envelope, in the constraints and the objective.
//!     Must precede RewriteAbs, which replaces the nodes it marks
//!   - RewriteAbs: detects ABS(expr) over decide vars, creates auxiliary REAL vars,
//!     replaces ABS nodes with aux var refs, generates linearization constraints
//!   - RewriteBilinear: McCormick linearization for Boolean × anything products
//!   - DefineNonlinearReducerBodies: names each non-linear body a MIN/MAX formulation would read by a helper decision
//!     per instance of its scope, defined on the side its clause pushes it (decide_rewrite_define.cpp)
//!   - RewriteScopedReducers: the value rewrite of a scoped comparison -- each decision
//!     reducer of a VALUE-shape comparison becomes a helper decision defined per group
//!   - RewriteComposedMinMax: detects composed MIN/MAX before the single-term pass
//!   - RewriteMinMax: classifies MIN/MAX constraints as easy/hard, rewrites to per-row
//!     or SUM+indicator, handles objectives (flat and two-level)
//!   - RewriteNotEqual: creates indicator variables for <> constraints
//!   - RewriteAvgToSum: rewrites AVG(expr) → SUM(expr) with alias tag for RHS scaling
//!   - FoldSingletonRows: folds simple `x OP const` bounds into the column box.
//!     Must run LAST — see its declaration.
//!
//! Builds the windows of the bounds the bound-side rewrite left to a rule (LogicalDecide::reduced_bounds) and puts
//! their checks in the scope checks. The solver path needs them; so does a direct-solve IR whose rules cannot compute a
//! bound themselves (DirectIR::bound_windows_needed).
void InstallReducedBoundWindows(Optimizer &optimizer, LogicalDecide &decide);

class DecideOptimizer {
public:
	explicit DecideOptimizer(Optimizer &optimizer);

	//! Recursively optimize the plan, transforming any LogicalDecide nodes found
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> plan);

private:
	//! Optimize the subtree held by `op`, which sits inside the plan held by `root`. A direct-solve rule that replaces
	//! a DECIDE node rewrites the references above it from `root`.
	void OptimizeSubtree(unique_ptr<LogicalOperator> &op, unique_ptr<LogicalOperator> &root);

	//! The bound-side evaluator: replaces each reducer on a bound by a window column of the DECIDE source (the same
	//! aggregate over its groups), reduces the bound of an aggregate-shape clause to its group's tightest value, and
	//! appends the VALUE_IS_VALID checks of what those windows read. The solver and the direct-solve rules then read
	//! one number that DuckDB computed. Runs first, in every mode, because both paths must see it. A bound a
	//! direct-solve rule can compute in its own group aggregate is left to it, recorded as the aggregates of its group
	//! (LogicalDecide::reduced_bounds).
	void RewriteRhsAggregates(LogicalDecide &decide);

	//! Builds the windows of the bounds RewriteRhsAggregates left to a rule (LogicalDecide::reduced_bounds). Runs when
	//! no direct-solve rule took the query, because only the solver path reads their columns.
	void InstallReducedBounds(LogicalDecide &decide);

	//! Optimize the DECIDE node held by `op`, which sits inside the plan held by `root`: pass 0, the choice of how it
	//! is answered, and for the solver the formulation passes. A rule that answers the query replaces `op`. The whole
	//! stage is one profile span and one DECIDB_BENCH timer, whichever way the query is answered.
	void OptimizeDecide(unique_ptr<LogicalOperator> &op, unique_ptr<LogicalOperator> &root);
	//! Installs the scope checks of the node that a plan can make under its child (DirectAddRowChecks), so the solver
	//! path and the direct-solve plans make them with one implementation; the operator keeps the rest.
	void InstallRowChecks(LogicalDecide &decide);
	//! The passes that formulate the query for the solver chosen, in order.
	void RewriteForSolver(LogicalDecide &decide);
	//! Before any one-to-many rewrite, assign written-order identities to source clauses
	//! whose formulations may only be removed as an atomic diagnostic repair.
	void TagAtomicRemovalGroups(LogicalDecide &decide);

	//! Lower NORM markers before ABS/MINMAX sees their generated expressions.
	void RewriteNorm(LogicalDecide &decide);
	//! Lower bound COMPARE_IN markers into the existing exact indicator formulation.
	void RewriteInDomain(LogicalDecide &decide);

	//! Rewrite not-equal (<>) constraints by creating auxiliary indicator variables.
	//! For each COMPARE_NOTEQUAL found in the constraint tree, creates a BOOLEAN
	//! indicator variable and records its index in ne_clause_sources.
	//! The actual Big-M constraints are generated at execution time when bounds are known.
	void RewriteNotEqual(LogicalDecide &decide);

	//! Helper: recursively find COMPARE_NOTEQUAL in bound expression tree
	void FindNotEqualConstraints(Expression &expr, LogicalDecide &decide);

	//! Fold every simple `x OP const` constraint (a BETWEEN is two of them by now) into the
	//! decision column's box (`singleton_row_boxes`, combined by `DecideFoldSingletonRow`) rather
	//! than leaving it to become a model row: a box is smaller and tighter for the solver than
	//! one row per data row. Folded comparisons are tagged SINGLETON_ROW_BOUND_TAG so physical
	//! term extraction skips them; a recorded one (`user_singleton_row_bounds`) is also tagged
	//! SINGLETON_ROW_CLAUSE_ROWS_TAG, so its clause is flattened for the repair model.
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

	//! Helper: recursively walk the constraint tree folding what it can. WHEN-wrapped
	//! comparisons are conditional, so they never contribute to a global bound.
	void FoldSingletonRowsInExpression(Expression &expr, LogicalDecide &decide);

	//! Rewrite decision-bearing AVG(expr) aggregates to SUM(expr) with alias tagging.
	//! Execution scales extracted AVG terms by the active row count.
	void RewriteAvgToSum(LogicalDecide &decide);

	//! Helper: recursively walk a bound expression tree, replacing AVG aggregates with SUM.
	//! The replacement is tagged with AVG_REWRITE_TAG so coefficient evaluation can scale terms.
	//! A decision-free AVG is skipped: there is nothing to linearize, and rebinding it as SUM
	//! would redeclare it with SUM's integral type while its value stays fractional.
	//! `clause_alias` is the clause the walk stands in (OBJECTIVE_ROW_TAG for the
	//! objective), under which each rewritten AVG's account is recorded.
	void RewriteAvgInExpression(unique_ptr<Expression> &expr, LogicalDecide &decide, const string &clause_alias);

	//! The value rewrite (language/guide.md §6.4): every decision reducer of a
	//! comparison in the VALUE shape (VALUE_REWRITE_TAG) becomes a helper decision defined
	//! once per `by` group (LogicalDecide::reducer_value_links), and the comparison is then
	//! one row per class. Runs after ABS and bilinear, whose auxiliaries may stand inside a
	//! reducer's body, and before the MIN/MAX passes, which never see these reducers. A
	//! product of decisions left inside such a reducer is refused (language/errors.md §2).
	void RewriteScopedReducers(LogicalDecide &decide);

	//! Detect composed MIN/MAX constraints — additive LHS mixing SUM/AVG/MIN/MAX terms.
	//! Extracts each term's metadata into decide.composed_minmax_constraints and replaces
	//! the comparison with a no-op placeholder so the physical layer owns emission.
	//! Must run before RewriteMinMax (which handles single top-level MIN/MAX).
	void RewriteComposedMinMax(LogicalDecide &decide);

	//! Helper: walk the constraint tree looking for composed MIN/MAX comparisons. `scope` encloses `expr`: its `when`
	//! is folded into a composed clause's terms, and its `per K:` names the classes the clause's one row stands for
	//! (D28).
	void RewriteComposedMinMaxInConstraint(unique_ptr<Expression> &expr, LogicalDecide &decide,
	                                       DecideEnclosingScope scope = DecideEnclosingScope());

	//! Detect a composed MIN/MAX objective (MIN/MAX terms beside other terms, or one under a factor).
	//! Extracts the MIN/MAX terms into decide.composed_minmax_objective_terms and leaves the
	//! others as the objective, a zero placeholder when there are none. Must run before
	//! RewriteMinMax (which handles single top-level MIN/MAX objectives).
	void RewriteComposedMinMaxObjectiveTop(LogicalDecide &decide);

	//! Rewrite MIN/MAX aggregates in constraints and objectives.
	//! Constraints: easy cases (MAX<=K, MIN>=K) keep the reducer, tagged, and bound each
	//! member; hard cases create an extremum column. Objectives: detect the flat and the
	//! two-level form, set metadata, rewrite to SUM.
	void RewriteMinMax(LogicalDecide &decide);

	//! Top-level constraint-side MIN/MAX rewrite
	void RewriteMinMaxConstraints(LogicalDecide &decide);

	//! Helper: recursively walk bound constraint tree, classifying and rewriting MIN/MAX. `scope` encloses `expr`; a
	//! row this rewrite adds for the clause (an equality's easy half) keeps its `when` and `per K:`.
	void RewriteMinMaxInConstraint(unique_ptr<Expression> &expr, LogicalDecide &decide,
	                               vector<unique_ptr<Expression>> &new_constraints,
	                               DecideEnclosingScope scope = DecideEnclosingScope());

	//! Objective-side MIN/MAX detection and rewriting.
	//! Handles the flat and the two-level objective.
	void RewriteMinMaxObjective(LogicalDecide &decide);
	//! The tree half of RewriteMinMaxObjective, split out so the objective can be
	//! detached, rewritten, and reinstalled through LogicalDecide::SetObjective. It
	//! cannot detach at the end instead: the rewrite writes through a pointer that
	//! walks INTO the tree past `per`/`when` wrappers and casts, and it has early returns.
	void RewriteMinMaxObjectiveTree(LogicalDecide &decide, unique_ptr<Expression> &objective);

	//! Detect ABS(expr) over decide variables, create auxiliary REAL variables,
	//! replace ABS nodes with aux var references, and generate linearization constraints.
	//! For each ABS(inner): aux >= inner AND aux >= -inner.
	void RewriteAbs(LogicalDecide &decide);

	//! Rewrite bilinear products (x * y) where at least one factor is Boolean.
	//! Boolean × Boolean: w = b1 * b2, AND-linearization (w <= b1, w <= b2, w >= b1+b2-1).
	//! Boolean × Other: w = b * x, partial McCormick (w <= x at plan time,
	//!   w <= U*b and w >= x-U*(1-b) at execution time via BilinearLink).
	//! Non-Boolean × Non-Boolean: left in place for Q matrix handling in physical operator.
	void RewriteBilinear(LogicalDecide &decide);

	//! Give each non-linear reducer body a MIN/MAX formulation would read as linear a helper decision `t` per row and
	//! one quadratic row: `t >= body` where the clause pushes the reducer down, `t <= body` up, `t = body` neither
	//! way (WalkPolarity). The reducer then reads the bare `t`, so every later pass sees linear members. Refuses on a
	//! backend that states no MIN/MAX natively: a helper has no box, so no Big-M exists for an extremum over it. Runs
	//! after RewriteBilinear, whose Boolean products are linear, and before the value and MIN/MAX rewrites.
	void DefineNonlinearReducerBodies(LogicalDecide &decide);

	//! Helper: recursively find bilinear products in expression tree, replace Boolean cases
	//! with auxiliary variable references, and collect metadata.
	//! `source_alias` is the alias of the enclosing user comparison, so the rows this
	//! emits can point back at the clause that produced them.
	void FindAndReplaceBilinear(unique_ptr<Expression> &expr, LogicalDecide &decide,
	                            vector<LogicalDecide::BilinearLink> &links, const string &source_alias);

	//! Carries per-ABS-node metadata through the two phases of RewriteAbs.
	struct AbsPairInfo {
		idx_t aux_idx;
		unique_ptr<Expression> inner_expr;
		//! Alias of the user comparison this ABS was written in, so the envelope rows
		//! it generates carry the clause id back to EXPLAIN and diagnostics; the
		//! objective's tag for an ABS in the objective.
		string source_alias;
		bool needs_bigm; // true = aux needs Big-M upper envelope (a constraint
		                 //        hard direction, an objective term the sense
		                 //        pushes up or of unknown sign, or an ABS nested
		                 //        inside another ABS). When false aux pins to
		                 //        |inner| naturally via solver pressure or a
		                 //        constraint shape that upper-bounds it.
		//! The envelope rows read only the rows the clause's `when` (a copy, or null) keeps.
		unique_ptr<Expression> when;
		//! The ABS as a plan prints it: the auxiliary's name there, and its account's.
		string label;
		//! The auxiliary's scope, ScopeOf its argument: its envelope rows are one per instance of it.
		DecideVarScopeInfo scope;
	};

	//! Helper: recursively find BoundFunctionExpression for ABS over decide vars,
	//! replace with auxiliary variable references, and collect AbsPairInfo entries,
	//! innermost first. Whether an auxiliary is pinned exactly is read off the tag
	//! TagAbsForBigM left on the node; this pass decides nothing about it.
	void FindAndReplaceAbs(unique_ptr<Expression> &expr, LogicalDecide &decide,
	                       const DecideCanonicalizer &canonicalizer, vector<AbsPairInfo> &abs_pairs,
	                       const string &source_alias, DecideEnclosingScope scope);

	//! Helper: register a hard MIN/MAX clause and produce the corresponding SUM(inner)
	//! aggregate tagged with MINMAX_CLAUSE_TAG_PREFIX. Records the clause's diagnosis
	//! text in decide.minmax_clause_labels and writes its index to out_clause_idx; the tag
	//! carries that index, which is how stage 06 names the column it pins. Allocates no
	//! variable — the binaries a Big-M pinning needs are global-block columns, created by
	//! the pass that emits the rows reading them.
	unique_ptr<Expression> EmitHardMinMaxClause(LogicalDecide &decide, const string &agg_name,
	                                            const Expression &inner, const Expression *filter,
	                                            idx_t &out_clause_idx);

	//! Helper: append a constraint to the decide constraint tree via AND conjunction
	//! Thin forwarder to LogicalDecide::AddConstraint, which canonicalizes on insert.
	//! Non-static because it needs the optimizer's ClientContext.
	void AppendConstraint(LogicalDecide &decide, unique_ptr<Expression> constraint);

	Optimizer &optimizer;
};

} // namespace duckdb
