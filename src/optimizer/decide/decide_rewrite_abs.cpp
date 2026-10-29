//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/optimizer/decide/decide_rewrite_abs.cpp
//
// DECIDE ABS handling: Big-M tagging and ABS linearization. See decide_optimizer.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/planner/decide/decide_cast_policy.hpp"

#include <cstdlib>
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/decide_solver_gate.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

// ---------------------------------------------------------------------------
// ABS soundness handling (must run BEFORE RewriteAbs)
// ---------------------------------------------------------------------------
//
// DecidB's ABS linearization replaces `ABS(e)` with an auxiliary `aux` and
// adds the lower-envelope constraints `aux >= e` and `aux >= -e`. That alone
// only forces `aux >= |e|`. Soundness then requires that aux be pinned to
// exactly |e|, not free above it. There are three pinning mechanisms:
//
//   1. Solver pressure: aux contributes to the objective with the sign the
//      sense pushes down -- positively under MINIMIZE, negatively under
//      MAXIMIZE -- and the solver pushes aux down to |e|. Sound — no Big-M
//      needed.
//   2. Constraint context that upper-bounds aux: ABS on the LHS of `<=`/`<`,
//      or on the RHS of `>=`/`>`. The constraint itself caps aux from
//      above, the lower envelope caps it from below at |e|. Sound — no
//      Big-M needed.
//   3. Big-M upper envelope with a sign-indicator binary y: emit
//      `aux <= e + 2M(1-y)` and `aux <= -e + 2M*y`. Combined with the
//      lower envelope this forces aux = |e| exactly, regardless of solver
//      pressure or constraint shape. Used for an objective ABS the sense
//      pushes up, and for any constraint shape outside category 2.
//
// This pass walks the constraint tree and the objective, classifies each
// ABS by the sign it carries, and tags the ABS function expressions that
// nothing pins with ABS_NEEDS_BIGM_TAG. RewriteAbs reads the tag and
// propagates to AbsPairInfo::needs_bigm; Phase 2 then allocates the y
// indicator and emits the upper-envelope at execution time (the existing
// AbsMaximizeLink path).
//
// The objective reads like a constraint: `MINIMIZE E` pins what `E <= K`
// pins and `MAXIMIZE E` what `E >= K` pins, so `MINIMIZE sum(abs(e))` and
// `MAXIMIZE -abs(e)` are held by pressure while `MAXIMIZE sum(abs(e))` and
// `MINIMIZE -abs(e)` are not. A term under a factor whose sign is unknown
// until the query runs (a data column weight) is not pinned either:
// `MINIMIZE sum(w * abs(e))` with one negative `w` was called unbounded.
//
// Aggregates over ABS (`SUM`, `AVG`, `MIN`, `MAX`) compose under the same
// rule. When the aggregate constraint upper-bounds the aggregate value
// (e.g. `SUM(ABS) <= K`, `MAX(ABS) <= K`), individual auxes are pinned
// transitively and Big-M is not needed. Otherwise we Big-M each aux and
// the aggregate then operates on pinned auxes. One exception: a MIN or MAX
// pushed its hard way (`MIN(ABS) <= K`, `MINIMIZE MIN(ABS)`) is encoded with
// a Big-M over its members, which needs every member's box, and an auxiliary
// held by pressure alone has none -- so an ABS under a hard extremum always
// takes the exact envelope, which boxes it.
// ---------------------------------------------------------------------------

static bool ContainsAbsOverDecideVar(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (StringUtil::CIEquals(func.function.name, "abs") && func.children.size() == 1) {
			if (BoundExpressionReferencesDecide(*func.children[0], decide_index)) {
				return true;
			}
		}
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = ContainsAbsOverDecideVar(child, decide_index);
		}
	});
	return found;
}

// Walk an expression tree and tag every BoundFunctionExpression for ABS over
// a decide var with ABS_NEEDS_BIGM_TAG, an ABS nested inside another included.
// Used on the side of a comparison that does not upper-bound aux (or on entire
// BETWEEN/IN/equality/<> subtrees).
static void TagAbsSubtreeForBigM(Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (StringUtil::CIEquals(func.function.name, "abs") && func.children.size() == 1) {
			if (BoundExpressionReferencesDecide(*func.children[0], decide_index)) {
				AddDecideTag(func.alias, ABS_NEEDS_BIGM_TAG);
			}
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { TagAbsSubtreeForBigM(child, decide_index); });
}

// Collect every ABS-over-decide-var node together with the sign it carries once
// the constraint is expanded additively (WalkPolarity). `pinning_sign` is the sign
// the relation or the sense pushes down (+1 under `<=` and MINIMIZE, -1 under `>=`
// and MAXIMIZE, 0 when nothing is pushed), which a MIN or MAX on the path needs to
// tell its cheap direction from its hard one.
//
// A factor whose sign is not known until execution -- a data column, as in
// `SUM(w * ABS(x - t))` -- yields sign 0, "unknown", which never matches the
// pinning direction and so forces Big-M. That is conservative: for `w >= 0` the
// aux would have been pinned without it. Deciding per row at execution time
// would recover those rows, but the indicator is allocated at plan time, so it
// cannot be chosen lazily as the code stands. A factor that is exactly zero makes
// the term vanish: nothing pushes its auxiliary anywhere, so nothing has to pin it,
// and asking for the exact envelope would demand a finite box the term has no use for.
static void CollectAbsWithSign(ClientContext &context, Expression &expr, int sign, idx_t decide_index,
                               vector<std::pair<Expression *, int>> &out, int pinning_sign) {
	PolarityHolds holds = [&](const Expression &node) {
		return ContainsAbsOverDecideVar(node, decide_index);
	};
	PolarityVisit visit = [&](Expression &node, int node_sign) {
		if (node.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &func = node.Cast<BoundFunctionExpression>();
			if (StringUtil::CIEquals(func.function.name, "abs") && func.children.size() == 1 &&
			    BoundExpressionReferencesDecide(*func.children[0], decide_index)) {
				// The ABS node itself is the unit that gets tagged. An ABS nested inside it
				// sits under a function that is not monotone in it, so no pressure and no
				// shape pins it: it is collected with sign 0, which never classifies as pinned.
				out.emplace_back(&node, node_sign);
				WalkPolarity(context, *func.children[0], 0, holds, visit);
				return true;
			}
			return false;
		}
		if (node.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return false;
		}
		// Aggregates compose: when the constraint bounds the aggregate's value in
		// the pinning direction, the auxes underneath are pinned transitively --
		// through a SUM or AVG, and through a MIN or MAX pushed its cheap way (a MAX
		// pushed down, a MIN pushed up), whose envelope every member bounds. Pushed
		// the other way an extremum is hard: it is encoded with a Big-M over its
		// members, which needs each member's box, and an auxiliary held by pressure
		// alone has none. So an ABS under a hard extremum reads as unknown sign and
		// takes the exact envelope, which boxes it: `MINIMIZE min(abs(x - 1))`,
		// `MAXIMIZE -min(abs(x - 1))`, `min(abs(x - 1)) <= K`.
		auto &agg = node.Cast<BoundAggregateExpression>();
		auto agg_name = StringUtil::Lower(agg.function.name);
		if ((agg_name != "min" && agg_name != "max") || node_sign == 0 || pinning_sign == 0) {
			return false;
		}
		const auto push = node_sign == pinning_sign ? ExtremumPush::DOWN : ExtremumPush::UP;
		if (IsEasyExtremum(agg_name == "max", push)) {
			return false;
		}
		for (auto &child : agg.children) {
			WalkPolarity(context, *child, 0, holds, visit);
		}
		return true;
	};
	WalkPolarity(context, expr, sign, holds, visit);
}

static void ClassifyAbsConstraints(ClientContext &context, Expression &expr, idx_t decide_index) {
	// A `when` or `per` scope only affects which rows participate in the aggregate, and aux
	// pinning is unconditional per row, so a wrapper does not change classification --
	// the ordinary constraint-position descent is exactly right here.
	ForEachConstraintLeaf(expr, [&](Expression &node) {
		if (node.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			auto &comp = node.Cast<BoundComparisonExpression>();
			bool lhs_has_abs = ContainsAbsOverDecideVar(*comp.left, decide_index);
			bool rhs_has_abs = ContainsAbsOverDecideVar(*comp.right, decide_index);
			if (!lhs_has_abs && !rhs_has_abs) {
				return;
			}
			auto t = comp.type;
			// Read the constraint as `E <op> 0` with `E = LHS - RHS`: every ABS is then
			// a single signed term of one expression, and "does this comparison push the
			// aux down?" is a question about that term's sign rather than about which
			// side of the relation it happens to be written on. That distinction is
			// invisible before canonicalization -- which never moved a term across the
			// relation until it ran ahead of this pass -- and load-bearing after it.
			int pinning_sign;
			if (t == ExpressionType::COMPARE_LESSTHAN || t == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
				pinning_sign = 1; // E bounded above: a positive term is pushed down
			} else if (t == ExpressionType::COMPARE_GREATERTHAN ||
			           t == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
				pinning_sign = -1; // E bounded below: a negative term is pushed down
			} else {
				pinning_sign = 0; // equality, <>: pin nothing, exactly as before
			}

			vector<std::pair<Expression *, int>> signed_abs;
			CollectAbsWithSign(context, *comp.left, 1, decide_index, signed_abs, pinning_sign);
			CollectAbsWithSign(context, *comp.right, -1, decide_index, signed_abs, pinning_sign);
			for (auto &entry : signed_abs) {
				if (pinning_sign == 0 || entry.second != pinning_sign) {
					TagAbsSubtreeForBigM(*entry.first, decide_index);
				}
			}
			return;
		}
		// Other top-level shapes (BETWEEN, IN, equality, <>): no per-side direction
		// analysis available — tag every ABS in the subtree for Big-M to be safe.
		if (ContainsAbsOverDecideVar(node, decide_index)) {
			TagAbsSubtreeForBigM(node, decide_index);
		}
	});
}

// The objective read as one signed expression under its sense. MINIMIZE pushes a
// positive term down, MAXIMIZE a negative one; every ABS whose sign is the other
// one, or is not known until the query runs, needs the exact envelope. This is the
// comparison rule above with the sense standing in for the relation.
static void ClassifyAbsObjective(ClientContext &context, Expression &objective, idx_t decide_index, DecideSense sense) {
	// A `when` or two-level `per` wrapper carries no sign: the body is its first child.
	Expression *body = &objective;
	while (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conjunction = body->Cast<BoundConjunctionExpression>();
		if (!IsConstraintWrapper(conjunction) || conjunction.children.empty()) {
			break;
		}
		body = conjunction.children[0].get();
	}
	if (!ContainsAbsOverDecideVar(*body, decide_index)) {
		return;
	}
	const int pinning_sign = sense == DecideSense::MINIMIZE ? 1 : -1;
	vector<std::pair<Expression *, int>> signed_abs;
	CollectAbsWithSign(context, *body, 1, decide_index, signed_abs, pinning_sign);
	for (auto &entry : signed_abs) {
		if (entry.second != pinning_sign) {
			TagAbsSubtreeForBigM(*entry.first, decide_index);
		}
	}
}

void decide_rewrite::TagAbsForBigM(ClientContext &context, LogicalDecide &decide) {
	if (decide.decide_constraints) {
		ClassifyAbsConstraints(context, *decide.decide_constraints, decide.decide_index);
	}
	if (decide.decide_objective) {
		ClassifyAbsObjective(context, *decide.decide_objective, decide.decide_index, decide.decide_sense);
	}
}

// ---------------------------------------------------------------------------
// ABS linearization (self-contained: detect, replace, and generate constraints)
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteAbs(LogicalDecide &decide) {
	// Phase 1: Find ABS(expr) nodes over decide vars, replace with auxiliary variables.
	// Each entry records the aux index, a copy of the inner expression, and the clause
	// the ABS node was written in (the objective's rows carry OBJECTIVE_ROW_TAG).
	vector<AbsPairInfo> abs_pairs;
	DecideCanonicalizer canonicalizer(optimizer.context, decide.decide_index, decide.variable_scopes,
	                                  decide.keyed_scopes, decide.is_boolean_var);
	if (decide.decide_constraints) {
		FindAndReplaceAbs(decide.decide_constraints, decide, canonicalizer, abs_pairs, string(),
		                  DecideEnclosingScope());
	}
	if (decide.decide_objective) {
		auto objective = std::move(decide.decide_objective);
		FindAndReplaceAbs(objective, decide, canonicalizer, abs_pairs, OBJECTIVE_ROW_TAG, DecideEnclosingScope());
		decide.SetObjective(optimizer.context, std::move(objective));
	}

	// Phase 2: Generate linearization constraints for each auxiliary variable.
	// Always emit the lower-bound envelope:
	//   aux >= inner  and  aux >= -inner
	// (forces aux >= |inner|). Then, for auxes that are NOT pinned to |inner|
	// by natural solver pressure or constraint shape, allocate a binary sign
	// indicator y and tag the lower-envelope constraints so physical_decide.cpp
	// can emit the upper envelope at execution time:
	//   aux <= inner  + 2M*(1-y)   (pins aux to inner  when y=1)
	//   aux <= -inner + 2M*y       (pins aux to -inner when y=0)
	//
	// Big-M is required when:
	//   - the objective's sense pushes the auxiliary up: a positive ABS term under
	//     MAXIMIZE, a negative one under MINIMIZE, or one under a factor whose sign
	//     is not known until the query runs (TagAbsForBigM reads the objective by
	//     sign, as it reads a constraint).
	//   - ABS appears in a constraint shape that does not upper-bound aux (the
	//     hard direction: ABS(...) >= K, ABS = K, ABS <> K, BETWEEN, etc.).
	//   - ABS sits inside another ABS: the outer one is not monotone in it, so no
	//     pressure or shape pins it (collected with sign 0 by TagAbsForBigM).
	//   Every one of these is decided in TagAbsForBigM and read here as pair.needs_bigm;
	//   a term under a factor of exactly zero is dead and gets no envelope at all.
	//
	// A positive ABS under MINIMIZE, a negative one under MAXIMIZE, and a sound
	// constraint shape (e.g. ABS <= K) skip the upper envelope — solver pressure /
	// direct upper-bounding handles pinning.
	for (idx_t pi = 0; pi < abs_pairs.size(); pi++) {
		auto &pair = abs_pairs[pi];
		auto &aux_var = decide.decide_variables[pair.aux_idx];
		auto &aux_ref = aux_var->Cast<BoundColumnRefExpression>();

		// Constraint 1 (C1): aux >= inner_expr
		auto aux_ref1 = make_uniq<BoundColumnRefExpression>(
		    aux_ref.alias, aux_ref.return_type, aux_ref.binding);
		auto c1 = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		    std::move(aux_ref1), pair.inner_expr->Copy());

		// Constraint 2 (C2): aux >= -inner_expr  (computed as 0 - inner_expr)
		auto aux_ref2 = make_uniq<BoundColumnRefExpression>(
		    aux_ref.alias, aux_ref.return_type, aux_ref.binding);
		auto neg_expr = optimizer.BindScalarFunction(
		    "-",
		    make_uniq<BoundConstantExpression>(Value::INTEGER(0)),
		    pair.inner_expr->Copy());
		auto c2 = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		    std::move(aux_ref2), std::move(neg_expr));

		const bool needs_bigm = pair.needs_bigm;
		// The account says how the auxiliary is pinned: on the sound direction solver
		// pressure does it; otherwise the lowering arm switches the envelope with a sign
		// indicator and the native arm states the absolute value outright.
		using Kind = LogicalDecide::RewriteAccount::Kind;
		AddRewriteAccount(decide, pair.source_alias,
		                  !needs_bigm                        ? Kind::ABS_AUXILIARY
		                  : decide.use_native_constructs.abs ? Kind::ABS_AUXILIARY_NATIVE
		                                                     : Kind::ABS_AUXILIARY_SIGN_INDICATOR,
		                  pair.label);
		if (needs_bigm) {
			// The sign indicator exists to switch the Big-M upper envelope, and the
			// native arm emits no Big-M: it states `aux = |t|` outright. So allocate y
			// only on the arm that has a use for it. Left in on the native arm it was one
			// free binary PER DATA ROW -- row-scoped, so it grew with the relation --
			// referenced by no row and no general constraint. Solvers presolve such a
			// column away, so it never changed an answer; it was still built, stored and
			// marshalled across the solver API for every row of the input.
			//
			// This is readable here only because stage 05 now OWNS the formulation
			// decision: the arm is known at the moment the variable would be allocated.
			idx_t y_idx = DConstants::INVALID_INDEX;
			if (!decide.use_native_constructs.abs) {
				// The sign of the auxiliary's argument: one per instance of the auxiliary.
				y_idx = AddAuxiliaryDecision(decide, "__abs_y_" + to_string(pi) + "__", LogicalType::BOOLEAN, true,
				                             pair.scope, "(" + RenderForPlan(*pair.inner_expr, decide) + " >= 0)");
			}

			// Tag C1 and C2 so the linearizer can find them at finalization. Keyed by the
			// AUXILIARY, which both arms have, rather than by y, which only the lowering
			// arm allocates.
			c1->alias = string(ABS_UB_POS_TAG_PREFIX) + to_string(pair.aux_idx) + "__";
			c2->alias = string(ABS_UB_NEG_TAG_PREFIX) + to_string(pair.aux_idx) + "__";
			// The linearizer finds these by substring, so the clause id rides alongside
			// rather than replacing the marker.
			CopyClauseProvenanceTags(pair.source_alias, *c1);
			CopyClauseProvenanceTags(pair.source_alias, *c2);

			decide.abs_maximize_links.push_back({pair.aux_idx, y_idx});
		} else {
			MarkFormulationConstraint(*c1, pair.source_alias);
			MarkFormulationConstraint(*c2, pair.source_alias);
		}

		AppendConstraint(decide,
		                 ScopeRowPerInstance(decide, ScopeHelperRow(std::move(c1), pair.when.get()), pair.scope));
		AppendConstraint(decide,
		                 ScopeRowPerInstance(decide, ScopeHelperRow(std::move(c2), pair.when.get()), pair.scope));
	}
}

void DecideOptimizer::FindAndReplaceAbs(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                        const DecideCanonicalizer &canonicalizer, vector<AbsPairInfo> &abs_pairs,
                                        const string &source_alias, DecideEnclosingScope scope) {
	if (!expr) {
		return;
	}
	auto clause_alias = DescendSourceAlias(*expr, source_alias);
	scope = EnterDecideScope(*expr, scope);

	if (expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr->Cast<BoundFunctionExpression>();
		if (StringUtil::CIEquals(func.function.name, "abs") && func.children.size() == 1) {
			if (BoundExpressionReferencesDecide(*func.children[0], decide.decide_index)) {
				// Declare the auxiliary as INTEGER when the inner expression is
				// integer-typed — |k| preserves integer-valuedness, so downstream
				// strict-inequality rewrites (`SUM(|...|) < K → <= K-1`) stay sound.
				// Without this, all ABS auxes are DOUBLE and the LHS-integer check
				// in ilp_model_builder would reject valid integer-valued strict cases.
				// A copy: the argument is rewritten below, and the node it was may go.
				const LogicalType inner_type = func.children[0]->return_type;
				bool inner_is_integer = inner_type.IsIntegral() ||
				                        inner_type.id() == LogicalTypeId::BOOLEAN;
				LogicalType aux_type =
				    inner_is_integer ? LogicalType::INTEGER : LogicalType::DOUBLE;

				// F6: a diagnosis, and a plan, name the auxiliary by the user's ABS(inner) --
				// rendered before the argument is rewritten, so a nested ABS keeps its text.
				string label = "abs(" + RenderForPlan(*func.children[0], decide) + ")";

				// Innermost first: an ABS inside this one becomes its own auxiliary now, so
				// the inner expression copied below refers to it rather than carrying an
				// ABS no later pass would lower (term extraction read `abs(h)` as `h`). Its
				// pair is appended before this one, which is the order the auxiliary-bound
				// derivation boxes them in: the outer range needs the inner box.
				FindAndReplaceAbs(func.children[0], decide, canonicalizer, abs_pairs, clause_alias, scope);

				// A column defined by its argument: one per instance of the scopes the argument reads.
				auto aux_scope = ScopeOf(decide, canonicalizer, *func.children[0]);
				idx_t aux_idx = AddAuxiliaryDecision(decide, "__abs_aux_" + to_string(abs_pairs.size()) + "__",
				                                     aux_type, false, aux_scope, label);

				// Read the Big-M marker TagAbsForBigM set on the BoundFunctionExpression
				// alias before the rewrite: the one place that decides exactness, for a
				// constraint and for the objective, for an ABS nested in another (which it
				// collects as unknown sign) and for a dead one (which it leaves untagged).
				const bool needs_bigm = HasDecideTag(func.alias, ABS_NEEDS_BIGM_TAG);

				// Stash the bound inner expression for constraint generation
				abs_pairs.push_back({aux_idx, func.children[0]->Copy(), clause_alias, needs_bigm,
				                     scope.Condition() ? scope.Condition()->Copy() : nullptr, std::move(label),
				                     aux_scope});

				// Replace ABS(inner) with aux var reference
				ReplaceWithDecision(optimizer.context, decide, aux_idx, expr);
				return;
			}
		}
	}

	// Recurse into children
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		FindAndReplaceAbs(child, decide, canonicalizer, abs_pairs, clause_alias, scope);
	});
}

} // namespace duckdb
