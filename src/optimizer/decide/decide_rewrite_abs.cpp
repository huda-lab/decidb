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
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

// ---------------------------------------------------------------------------
// ABS-in-constraint soundness handling (must run BEFORE RewriteAbs)
// ---------------------------------------------------------------------------
//
// DecidB's ABS linearization replaces `ABS(e)` with an auxiliary `aux` and
// adds the lower-envelope constraints `aux >= e` and `aux >= -e`. That alone
// only forces `aux >= |e|`. Soundness then requires that aux be pinned to
// exactly |e|, not free above it. There are three pinning mechanisms:
//
//   1. Solver pressure under MINIMIZE objective: aux contributes positively
//      to the objective, the solver pushes aux down to |e|. Sound — no
//      Big-M needed.
//   2. Constraint context that upper-bounds aux: ABS on the LHS of `<=`/`<`,
//      or on the RHS of `>=`/`>`. The constraint itself caps aux from
//      above, the lower envelope caps it from below at |e|. Sound — no
//      Big-M needed.
//   3. Big-M upper envelope with a sign-indicator binary y: emit
//      `aux <= e + 2M(1-y)` and `aux <= -e + 2M*y`. Combined with the
//      lower envelope this forces aux = |e| exactly, regardless of solver
//      pressure or constraint shape. Used for MAXIMIZE objective ABS, and
//      now for any constraint shape outside category 2.
//
// This pass walks the constraint tree, classifies each ABS-bearing
// comparison, and tags ABS function expressions in non-category-2 positions
// with ABS_NEEDS_BIGM_TAG. RewriteAbs reads the tag and propagates to
// AbsPairInfo::needs_bigm; Phase 2 then allocates the y indicator and emits
// the upper-envelope at execution time (the existing AbsMaximizeLink path).
//
// Aggregates over ABS (`SUM`, `AVG`, `MIN`, `MAX`) compose under the same
// rule. When the aggregate constraint upper-bounds the aggregate value
// (e.g. `SUM(ABS) <= K`, `MAX(ABS) <= K`), individual auxes are pinned
// transitively and Big-M is not needed. Otherwise we Big-M each aux and
// the aggregate then operates on pinned auxes.
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
// a decide var with ABS_NEEDS_BIGM_TAG. Used on the side of a comparison that
// does not upper-bound aux (or on entire BETWEEN/IN/equality/<> subtrees).
static void TagAbsForBigM(Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (StringUtil::CIEquals(func.function.name, "abs") && func.children.size() == 1) {
			if (BoundExpressionReferencesDecide(*func.children[0], decide_index)) {
				AddDecideTag(func.alias, ABS_NEEDS_BIGM_TAG);
				return;
			}
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		TagAbsForBigM(child, decide_index);
	});
}

// Sign of a complete foldable expression. Data casts are evaluated, never peeled.
// Returns false when the expression is not a statically-known number.
static bool TryGetConstantSign(ClientContext &context, const Expression &expr, int &out_sign) {
	double value;
	if (!TryEvaluateFoldableDoubleNoThrow(context, expr, value)) {
		return false;
	}
	out_sign = value < 0 ? -1 : 1;
	return true;
}

// Collect every ABS-over-decide-var node together with the sign it carries once
// the constraint is expanded additively. `sign` is the sign of the path taken to
// reach the current node.
//
// Only structure whose sign is known at plan time is traversed with a flip: `+`,
// binary and unary `-`, casts, aggregate bodies, and numeric literal factors. A
// factor whose sign is not known until execution -- a data column, as in
// `SUM(w * ABS(x - t))` -- yields sign 0, "unknown", which never matches the
// pinning direction and so forces Big-M. That is conservative: for `w >= 0` the
// aux would have been pinned without it. Deciding per row at execution time
// would recover those rows, but the indicator is allocated at plan time, so it
// cannot be chosen lazily as the code stands.
static void CollectAbsWithSign(ClientContext &context, Expression &expr, int sign, idx_t decide_index,
                               vector<std::pair<Expression *, int>> &out) {
	if (!ContainsAbsOverDecideVar(expr, decide_index)) {
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		CollectAbsWithSign(context, *expr.Cast<BoundCastExpression>().child, sign, decide_index, out);
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		auto name = StringUtil::Lower(func.function.name);
		if (StringUtil::CIEquals(name, "abs") && func.children.size() == 1 &&
		    BoundExpressionReferencesDecide(*func.children[0], decide_index)) {
			// The ABS node itself is the unit that gets tagged; a nested ABS inside
			// it is subsumed, exactly as TagAbsForBigM stops here.
			out.emplace_back(&expr, sign);
			return;
		}
		if (name == "+") {
			for (auto &child : func.children) {
				CollectAbsWithSign(context, *child, sign, decide_index, out);
			}
			return;
		}
		if (name == "-" && func.children.size() == 2) {
			CollectAbsWithSign(context, *func.children[0], sign, decide_index, out);
			CollectAbsWithSign(context, *func.children[1], -sign, decide_index, out);
			return;
		}
		if (name == "-" && func.children.size() == 1) {
			CollectAbsWithSign(context, *func.children[0], -sign, decide_index, out);
			return;
		}
		if (name == "*" || name == "/") {
			// Fold the sign of every constant factor. For division only the numerator
			// can carry an ABS, but the divisor's sign still applies.
			//
			// A factor that is not a statically-known number and does not itself carry
			// the ABS is a data-dependent multiplier -- `w` in `SUM(w * ABS(x - t))`.
			// Its sign is unknown until execution, so the whole path sign becomes 0.
			// An ABS-bearing factor is the term being collected rather than a
			// multiplier, and is non-negative by construction, so it never flips.
			int factor_sign = 1;
			for (auto &child : func.children) {
				int child_sign;
				if (TryGetConstantSign(context, *child, child_sign)) {
					factor_sign *= child_sign;
				} else if (!ContainsAbsOverDecideVar(*child, decide_index)) {
					factor_sign = 0;
				}
			}
			for (auto &child : func.children) {
				int ignored;
				if (!TryGetConstantSign(context, *child, ignored)) {
					CollectAbsWithSign(context, *child, sign * factor_sign, decide_index, out);
				}
			}
			return;
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		// Aggregates compose: when the constraint bounds the aggregate's value in
		// the pinning direction, the auxes underneath are pinned transitively.
		for (auto &child : expr.Cast<BoundAggregateExpression>().children) {
			CollectAbsWithSign(context, *child, sign, decide_index, out);
		}
		return;
	}
	// Anything else (subqueries, CASE, operators with no sign discipline): the
	// sign of the path is unknown, so the safe answer is "not pinned". Hand the
	// contained ABS nodes back with sign 0, which never classifies as pinned.
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		CollectAbsWithSign(context, child, 0, decide_index, out);
	});
}

static void ClassifyAbsConstraints(ClientContext &context, Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		// WHEN/PER wrappers: child[0] is the inner constraint; recurse into it.
		// The WHEN/PER filter only affects which rows participate in the
		// aggregate; aux pinning is unconditional per row, so the wrapper
		// doesn't change classification.
		if (HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG) || IsPerConstraintTag(conj.alias)) {
			if (!conj.children.empty()) {
				ClassifyAbsConstraints(context, *conj.children[0], decide_index);
			}
			return;
		}
		for (auto &child : conj.children) {
			ClassifyAbsConstraints(context, *child, decide_index);
		}
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto &comp = expr.Cast<BoundComparisonExpression>();
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
		CollectAbsWithSign(context, *comp.left, 1, decide_index, signed_abs);
		CollectAbsWithSign(context, *comp.right, -1, decide_index, signed_abs);
		for (auto &entry : signed_abs) {
			if (pinning_sign == 0 || entry.second != pinning_sign) {
				TagAbsForBigM(*entry.first, decide_index);
			}
		}
		return;
	}
	// Other top-level shapes (BETWEEN, IN, equality, <>, conjunctions handled
	// above): no per-side direction analysis available — tag every ABS in the
	// subtree for Big-M to be safe.
	if (ContainsAbsOverDecideVar(expr, decide_index)) {
		TagAbsForBigM(expr, decide_index);
	}
}

void decide_rewrite::TagAbsConstraintsForBigM(ClientContext &context, LogicalDecide &decide) {
	if (!decide.decide_constraints) {
		return;
	}
	ClassifyAbsConstraints(context, *decide.decide_constraints, decide.decide_index);
}

// ---------------------------------------------------------------------------
// ABS linearization (self-contained: detect, replace, and generate constraints)
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteAbs(LogicalDecide &decide) {
	// Phase 1: Find ABS(expr) nodes over decide vars, replace with auxiliary variables.
	// Each entry records the aux index, a copy of the inner expression, and whether
	// the ABS node originated in the objective (vs. a constraint).
	vector<AbsPairInfo> abs_pairs;
	if (decide.decide_constraints) {
		FindAndReplaceAbs(decide.decide_constraints, decide, abs_pairs, /*in_objective=*/false, string());
	}
	if (decide.decide_objective) {
		auto objective = std::move(decide.decide_objective);
		FindAndReplaceAbs(objective, decide, abs_pairs, /*in_objective=*/true, string());
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
	//   - ABS appears in the objective with sense==MAXIMIZE (solver pushes aux up).
	//   - ABS appears in a constraint shape that does not upper-bound aux (the
	//     hard direction: ABS(...) >= K, ABS = K, ABS <> K, BETWEEN, etc.).
	//     These auxes are flagged via pair.needs_bigm by TagAbsConstraintsForBigM.
	//
	// MINIMIZE objective and sound constraint shapes (e.g. ABS <= K) skip the
	// upper envelope — solver pressure / direct upper-bounding handles pinning.
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

		bool needs_bigm = pair.needs_bigm ||
		                  (pair.in_objective && decide.decide_sense == DecideSense::MAXIMIZE);
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
				y_idx = decide.decide_variables.size();
				string y_name = "__abs_y_" + to_string(pi) + "__";
				auto y_var = make_uniq<BoundColumnRefExpression>(
				    y_name, LogicalType::BOOLEAN,
				    ColumnBinding(decide.decide_index, y_idx));
				decide.decide_variables.push_back(std::move(y_var));
				decide.num_auxiliary_vars++;
				decide.is_boolean_var.push_back(true);
				if (!decide.variable_scopes.empty()) {
					decide.variable_scopes.push_back(DecideVarScopeInfo::Row());
				}
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

		AppendConstraint(decide, std::move(c1));
		AppendConstraint(decide, std::move(c2));
	}
}

void DecideOptimizer::FindAndReplaceAbs(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                        vector<AbsPairInfo> &abs_pairs, bool in_objective,
                                        const string &source_alias) {
	if (!expr) {
		return;
	}
	auto clause_alias = DescendSourceAlias(*expr, source_alias);

	if (expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr->Cast<BoundFunctionExpression>();
		if (StringUtil::CIEquals(func.function.name, "abs") && func.children.size() == 1) {
			if (BoundExpressionReferencesDecide(*func.children[0], decide.decide_index)) {
				// Declare the auxiliary as INTEGER when the inner expression is
				// integer-typed — |k| preserves integer-valuedness, so downstream
				// strict-inequality rewrites (`SUM(|...|) < K → <= K-1`) stay sound.
				// Without this, all ABS auxes are DOUBLE and the LHS-integer check
				// in ilp_model_builder would reject valid integer-valued strict cases.
				auto &inner_type = func.children[0]->return_type;
				bool inner_is_integer = inner_type.IsIntegral() ||
				                        inner_type.id() == LogicalTypeId::BOOLEAN;
				LogicalType aux_type =
				    inner_is_integer ? LogicalType::INTEGER : LogicalType::DOUBLE;

				// Create auxiliary variable
				idx_t aux_idx = decide.decide_variables.size();
				string aux_name = "__abs_aux_" + to_string(abs_pairs.size()) + "__";
				auto aux_var = make_uniq<BoundColumnRefExpression>(
				    aux_name, aux_type,
				    ColumnBinding(decide.decide_index, aux_idx));
				decide.decide_variables.push_back(std::move(aux_var));
				decide.num_auxiliary_vars++;
				decide.is_boolean_var.push_back(false);
				if (!decide.variable_scopes.empty()) {
					decide.variable_scopes.push_back(DecideVarScopeInfo::Row());
				}
				// F6: record the user's original ABS(inner) for diagnosis naming
				decide.aux_var_expressions.emplace_back(
				    aux_idx, "ABS(" + func.children[0]->ToString() + ")");

				// Read the Big-M marker set by TagAbsConstraintsForBigM. Tag is
				// set on the BoundFunctionExpression alias before the rewrite.
				// Constraint context owns the tag; objective ABS sets needs_bigm
				// independently below in Phase 2 based on sense.
				bool needs_bigm = HasDecideTag(func.alias, ABS_NEEDS_BIGM_TAG);

				// Stash the bound inner expression for constraint generation
				abs_pairs.push_back({aux_idx, func.children[0]->Copy(), in_objective, clause_alias, needs_bigm});

				// Replace ABS(inner) with aux var reference
				expr = make_uniq<BoundColumnRefExpression>(
				    aux_name, aux_type,
				    ColumnBinding(decide.decide_index, aux_idx));
				return;
			}
		}
	}

	// Recurse into children
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		FindAndReplaceAbs(child, decide, abs_pairs, in_objective, clause_alias);
	});
}

} // namespace duckdb
