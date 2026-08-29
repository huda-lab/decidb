//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/optimizer/decide/decide_rewrite_minmax.cpp
//
// DECIDE MIN/MAX rewrites, plain and composed. See decide_optimizer.cpp.
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
// Composed MIN/MAX constraints (additive LHS mixing SUM/AVG/MIN/MAX terms)
// ---------------------------------------------------------------------------
//
// Single-term `MIN/MAX(expr) CMP K` is handled by RewriteMinMax below. When a
// MIN/MAX appears *inside* an additive LHS (e.g. `SUM(a*x) + MAX(b*x) <= K`),
// we extract the full constraint shape into decide.composed_minmax_constraints
// and replace the comparison with a TRUE placeholder. The physical layer
// allocates global auxiliaries (z_k per MIN/MAX term) and emits the pinning
// constraints at sink-finalize time.

void DecideOptimizer::RewriteComposedMinMax(LogicalDecide &decide) {
	if (decide.decide_constraints) {
		RewriteComposedMinMaxInConstraint(decide.decide_constraints, decide);
	}
	RewriteComposedMinMaxObjectiveTop(decide);
}

// True if the expression is a BOUND_FUNCTION for `+` (after unwrapping cast).
static bool IsAddNode(const Expression &e, idx_t decide_index) {
	auto &u = (*UnwrapDecideCasts(e, decide_index));
	if (u.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) return false;
	return StringUtil::Lower(u.Cast<BoundFunctionExpression>().function.name) == "+";
}

// True if the expression is a `-` function (both binary and unary).
static bool IsSubNode(const Expression &e, idx_t decide_index) {
	auto &u = (*UnwrapDecideCasts(e, decide_index));
	if (u.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) return false;
	return StringUtil::Lower(u.Cast<BoundFunctionExpression>().function.name) == "-";
}

// True if any MIN/MAX aggregate over a decide var appears at or below the node.
// Recurses through any function node's children (not just +/-), so shapes like
// `2 * MIN(...)` are detected and can be rejected with a clean binder error.
static bool AdditiveContainsMinMax(const Expression &e, idx_t decide_index) {
	auto &u = (*UnwrapDecideCasts(e, decide_index));
	if (u.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &agg = u.Cast<BoundAggregateExpression>();
		auto name = StringUtil::Lower(agg.function.name);
		if ((name == "min" || name == "max") && agg.children.size() == 1 &&
		    BoundExpressionReferencesDecide(*agg.children[0], decide_index)) {
			return true;
		}
	}
	if (u.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &fn = u.Cast<BoundFunctionExpression>();
		for (auto &child : fn.children) {
			if (AdditiveContainsMinMax(*child, decide_index)) {
				return true;
			}
		}
	}
	return false;
}

// Walk the additive LHS, emitting a ComposedMinMaxTerm for each leaf aggregate.
// Throws BinderException on v1-unsupported shapes (non-aggregate leaves,
// nested subtraction/scaling, non-SUM/AVG/MIN/MAX aggregates).
static void WalkComposedLhs(ClientContext &context, const Expression &e, int sign, idx_t decide_index,
                             bool outer_push_down,
                             vector<LogicalDecide::ComposedMinMaxTerm> &out_terms) {
	auto &u = (*UnwrapDecideCasts(e, decide_index));
	if (IsAddNode(u, decide_index)) {
		auto &fn = u.Cast<BoundFunctionExpression>();
		for (auto &child : fn.children) {
			WalkComposedLhs(context, *child, sign, decide_index, outer_push_down, out_terms);
		}
		return;
	}
	if (IsSubNode(u, decide_index)) {
		// Subtraction flips the direction each term is pushed. `sign` already
		// carries that through to the easy/hard classification below, and the
		// physical layer is sign-generic, so descending is all that is required.
		// This has to work: canonicalization moves decision terms onto the LHS,
		// and a term that crosses the relation arrives negated.
		auto &fn = u.Cast<BoundFunctionExpression>();
		if (fn.children.size() == 2) {
			WalkComposedLhs(context, *fn.children[0], sign, decide_index, outer_push_down, out_terms);
			WalkComposedLhs(context, *fn.children[1], -sign, decide_index, outer_push_down, out_terms);
			return;
		}
		if (fn.children.size() == 1) {
			WalkComposedLhs(context, *fn.children[0], -sign, decide_index, outer_push_down, out_terms);
			return;
		}
	}
	// A zero constant contributes nothing. It reaches here as the head of the
	// canonicalizer's `0 - term` idiom for a leading negative term, so rejecting
	// it would reject shapes purely on how the negation was spelled.
	if (u.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		auto &val = u.Cast<BoundConstantExpression>().value;
		if (!val.IsNull() && val.type().IsNumeric() && val.GetValue<double>() == 0.0) {
			return;
		}
	}
	// A factor on this term (`2 * MIN(...)`) rides alongside it rather than being
	// pushed into the aggregate. Its sign joins the easy/hard classification below.
	ScaledAggregateMatch scale_match;
	const BoundAggregateExpression *scaled_agg =
	    TryMatchScaledAggregate(u, decide_index, scale_match) ? scale_match.aggregate : nullptr;
	auto scale = scale_match.scale;
	bool scale_divides = scale_match.divides;

	if (!scaled_agg && u.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		throw BinderException(
		    "Composed MIN/MAX in DECIDE v1 supports only additive sums of SUM/AVG/MIN/MAX aggregates. "
		    "Got non-aggregate term: %s",
		    e.ToString());
	}
	auto &agg = scaled_agg ? *scaled_agg : u.Cast<BoundAggregateExpression>();
	auto name = StringUtil::Lower(agg.function.name);
	if (name != "sum" && name != "avg" && name != "min" && name != "max") {
		throw BinderException("Composed MIN/MAX in DECIDE v1 does not support aggregate '%s'; "
		                      "only SUM/AVG/MIN/MAX are supported.", name);
	}
	if (agg.children.size() != 1) {
		throw BinderException("Composed MIN/MAX: aggregate '%s' must have a single inner expression.", name);
	}

	LogicalDecide::ComposedMinMaxTerm term;
	term.kind = (name == "min" || name == "max")
	                ? LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND
	                : LogicalDecide::ComposedMinMaxTerm::SUM_KIND;
	term.agg_name = name;
	term.sign = sign;
	term.inner_expr = agg.children[0]->Copy();
	if (agg.filter) {
		term.filter = agg.filter->Copy();
	}
	if (scale) {
		term.scale = scale->Copy();
		term.scale_divides = scale_divides;
	}
	// Carry the relation qualifier (`SUM(D: ...)`) off the tag the binder stamped on the
	// aggregate. Without this the composed path reduces over join-result rows and a
	// qualified reducer silently reverts to row semantics.
	TryParseQualifiedReducerTag(agg.alias, term.qualifier_scope_idx);
	if (term.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) {
		bool is_max = (name == "max");
		// A negative FACTOR flips which way z is pushed exactly as a subtraction does,
		// so the two combine into one effective sign. `-2 * MAX(e)` under `<=` pushes
		// MAX up, the hard direction, just as `- MAX(e)` would.
		int scale_sign = ScaleSignAtPlanTime(context, scale, scale_divides);
		if (scale_sign == 0) {
			// The factor's sign is not known until the query runs, so neither is the
			// cheap direction. The indicator layer pins z to the true MIN/MAX in BOTH
			// directions, which is correct for either sign -- pay for it rather than
			// guess. This is the case that used to be rejected outright.
			term.is_easy = false;
		} else {
			int effective_sign = sign * scale_sign;
			// z_k pushed down if the outer wants LHS small and this term's effective
			// sign is +, or outer wants LHS large and it is -.
			bool z_pushed_down = (effective_sign > 0) ? outer_push_down : !outer_push_down;
			// Easy: MAX pushed down, or MIN pushed up.
			term.is_easy = (is_max && z_pushed_down) || (!is_max && !z_pushed_down);
		}
	}
	out_terms.push_back(std::move(term));
}

void DecideOptimizer::RewriteComposedMinMaxInConstraint(unique_ptr<Expression> &expr, LogicalDecide &decide) {
	if (!expr) {
		return;
	}

	// Walk through AND conjunctions and WHEN/PER wrappers (no composed MIN/MAX inside WHEN/PER in v1).
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr->Cast<BoundConjunctionExpression>();
		if (HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG) || IsPerConstraintTag(conj.alias)) {
			// If the wrapped constraint is composed MIN/MAX, reject in v1.
			if (!conj.children.empty()) {
				auto &inner = *conj.children[0];
				if (inner.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
					auto &cmp = inner.Cast<BoundComparisonExpression>();
					if (cmp.left && AdditiveContainsMinMax(*cmp.left, decide.decide_index) &&
					    IsAddNode(*cmp.left, decide.decide_index)) {
						throw BinderException(
						    "Composed MIN/MAX in DECIDE v1 does not support outer WHEN/PER wrappers. "
						    "Remove the WHEN/PER or restructure the constraint.");
					}
				}
				RewriteComposedMinMaxInConstraint(conj.children[0], decide);
			}
			return;
		}
		// Regular AND conjunction — recurse into all children
		for (auto &child : conj.children) {
			RewriteComposedMinMaxInConstraint(child, decide);
		}
		return;
	}

	if (expr->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
		return;
	}
	auto &comp = expr->Cast<BoundComparisonExpression>();
	if (!comp.left) {
		return;
	}

	// Additive (+/-) with a MIN/MAX leaf, OR a lone MIN/MAX whose factor has a sign
	// this stage cannot know.
	//
	// The second case matters because the single-term rewrite has only two shapes: an
	// "easy" per-row fan-out (`∀i e_i <= K`) and a "hard" indicator form (`∃i e_i >=
	// K`). Those encode OPPOSITE quantifiers, so choosing between them requires the
	// factor's sign — with the sign unknown there is no safe default, and guessing
	// "hard" silently drops the ∀ half. The composed path emits the envelope pin AND
	// the indicator layer, which together pin the auxiliary to the true MIN/MAX in both
	// directions, so multiplying it by a factor of either sign stays exact.
	ScaledAggregateMatch lone_match;
	const BoundAggregateExpression *lone_scaled =
	    TryMatchScaledAggregate(*comp.left, decide.decide_index, lone_match) ? lone_match.aggregate : nullptr;
	auto lone_scale = lone_match.scale;
	bool lone_divides = lone_match.divides;
	bool unknown_sign_lone_minmax =
	    lone_scaled && lone_scale && ScaleSignAtPlanTime(optimizer.context, lone_scale, lone_divides) == 0 &&
	    (StringUtil::Lower(lone_scaled->function.name) == "min" ||
	     StringUtil::Lower(lone_scaled->function.name) == "max") &&
	    lone_scaled->children.size() == 1 &&
	    BoundExpressionReferencesDecide(*lone_scaled->children[0], decide.decide_index) &&
	    !BoundExpressionReferencesDecide(*lone_scale, decide.decide_index);

	if (!unknown_sign_lone_minmax) {
		if (!IsAddNode(*comp.left, decide.decide_index) && !IsSubNode(*comp.left, decide.decide_index)) {
			return;
		}
		if (!AdditiveContainsMinMax(*comp.left, decide.decide_index)) {
			return;
		}
	}

	auto cmp_type = comp.type;
	if (cmp_type != ExpressionType::COMPARE_LESSTHAN &&
	    cmp_type != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
	    cmp_type != ExpressionType::COMPARE_GREATERTHAN &&
	    cmp_type != ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
		// BETWEEN never reaches here — it is already a pair of directional
		// comparisons by this point — so the message must not claim otherwise;
		// for '=' it is also the smallest edit that works.
		if (cmp_type == ExpressionType::COMPARE_EQUAL) {
			throw BinderException("Composed MIN/MAX in DECIDE v1 does not support '='. "
			                      "Write the bound as BETWEEN K AND K.");
		}
		throw BinderException("Composed MIN/MAX in DECIDE v1 supports only the <, <=, >, >= and "
		                      "BETWEEN comparisons.");
	}

	bool outer_push_down = (cmp_type == ExpressionType::COMPARE_LESSTHAN ||
	                         cmp_type == ExpressionType::COMPARE_LESSTHANOREQUALTO);

	LogicalDecide::ComposedMinMaxConstraint spec;
	spec.outer_cmp = cmp_type;
	spec.rhs_expr = comp.right->Copy();
	TryParseSourceClauseTag(comp.GetAlias(), spec.source_clause_id);
	TryParseRemovalGroupTag(comp.GetAlias(), spec.removal_group_id);

	WalkComposedLhs(optimizer.context, *comp.left, /*sign=*/1, decide.decide_index, outer_push_down,
	                spec.terms);

	decide.composed_minmax_constraints.push_back(std::move(spec));

	// Replace the comparison with a TRUE placeholder so the normal constraint path is a
	// no-op. It carries the clause id so the plan still knows where the clause was written.
	expr = MakeTrueExpression(comp.GetAlias());
}

void DecideOptimizer::RewriteComposedMinMaxObjectiveTop(LogicalDecide &decide) {
	if (!decide.decide_objective) {
		return;
	}
	auto &obj = *decide.decide_objective;

	// Reject composed MIN/MAX in objectives with outer PER or WHEN (v1 scope).
	if (obj.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = obj.Cast<BoundConjunctionExpression>();
		if (IsPerConstraintTag(conj.alias) || HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG)) {
			// If the wrapped objective is composed, reject.
			if (!conj.children.empty()) {
				auto &inner = *conj.children[0];
				if (AdditiveContainsMinMax(inner, decide.decide_index) &&
				    (IsAddNode(inner, decide.decide_index) || IsSubNode(inner, decide.decide_index))) {
					throw BinderException(
					    "Composed MIN/MAX in DECIDE v1 does not support outer WHEN/PER "
					    "wrappers on the objective. Restructure the objective.");
				}
			}
			return;
		}
	}

	// Additive (+/-) with a MIN/MAX leaf, OR a lone MIN/MAX carrying a factor.
	//
	// The second case is here rather than on the flat path because the flat path
	// replaces the whole objective with its auxiliary at coefficient 1.0 -- it has
	// nowhere to put a factor. The composed path already carries one per term, and a
	// one-term composition is a perfectly good degenerate case. An UNSCALED lone
	// MIN/MAX still goes to the flat path, which keeps its cheaper encoding.
	ScaledAggregateMatch lone_match;
	const BoundAggregateExpression *lone_scaled =
	    TryMatchScaledAggregate(obj, decide.decide_index, lone_match) ? lone_match.aggregate : nullptr;
	auto lone_scale = lone_match.scale;
	bool is_scaled_lone_minmax =
	    lone_scaled && lone_scale &&
	    (StringUtil::Lower(lone_scaled->function.name) == "min" ||
	     StringUtil::Lower(lone_scaled->function.name) == "max") &&
	    lone_scaled->children.size() == 1 &&
	    BoundExpressionReferencesDecide(*lone_scaled->children[0], decide.decide_index) &&
	    !BoundExpressionReferencesDecide(*lone_scale, decide.decide_index);

	if (!is_scaled_lone_minmax) {
		if (!IsAddNode(obj, decide.decide_index) && !IsSubNode(obj, decide.decide_index)) {
			return;
		}
		if (!AdditiveContainsMinMax(obj, decide.decide_index)) {
			return;
		}
	}

	// Direction: MAXIMIZE pushes each term UP; MINIMIZE pushes each term DOWN.
	bool outer_push_down = (decide.decide_sense == DecideSense::MINIMIZE);

	vector<LogicalDecide::ComposedMinMaxTerm> terms;
	WalkComposedLhs(optimizer.context, obj, /*sign=*/1, decide.decide_index, outer_push_down, terms);

	decide.composed_minmax_objective_terms = std::move(terms);

	// Replace the objective with a zero placeholder. The physical layer fills in
	// objective coefficients from the spec.
	decide.SetObjective(optimizer.context, make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
}

// ---------------------------------------------------------------------------
// MIN/MAX linearization
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteMinMax(LogicalDecide &decide) {
	RewriteMinMaxConstraints(decide);
	RewriteMinMaxObjective(decide);
}

void DecideOptimizer::RewriteMinMaxConstraints(LogicalDecide &decide) {
	if (!decide.decide_constraints) {
		return;
	}
	vector<unique_ptr<Expression>> new_constraints;
	bool was_easy = false;
	RewriteMinMaxInConstraint(decide.decide_constraints, decide, new_constraints, was_easy);

	// Append generated constraints (from equality splitting) to the constraint tree
	for (auto &nc : new_constraints) {
		AppendConstraint(decide, std::move(nc));
	}
}

void DecideOptimizer::RewriteMinMaxInConstraint(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                                vector<unique_ptr<Expression>> &new_constraints,
                                                bool &out_was_easy) {
	if (!expr) {
		return;
	}

	// Handle WHEN/PER wrappers
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr->Cast<BoundConjunctionExpression>();
		if (HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG)) {
			// Recurse into the wrapped constraint (child[0])
			if (!conj.children.empty()) {
				RewriteMinMaxInConstraint(conj.children[0], decide, new_constraints, out_was_easy);
			}
			return;
		}
		if (IsPerConstraintTag(conj.alias)) {
			// Recurse into the wrapped constraint (child[0])
			if (!conj.children.empty()) {
				RewriteMinMaxInConstraint(conj.children[0], decide, new_constraints, out_was_easy);
				// Easy MIN/MAX (e.g., MAX(e) <= C, MIN(e) >= C) are vacuously true over
				// empty sets. Strip PER — the per-row form skips WHEN-excluded rows.
				if (out_was_easy) {
					expr = std::move(conj.children[0]);
				}
			}
			return;
		}
		// Regular AND conjunction — recurse into all children
		for (auto &child : conj.children) {
			bool child_easy = false;
			RewriteMinMaxInConstraint(child, decide, new_constraints, child_easy);
		}
		return;
	}

	// Check for comparison with MIN/MAX on LHS
	if (expr->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
		return;
	}
	auto &comp = expr->Cast<BoundComparisonExpression>();

	// Unwrap any BoundCastExpression on the LHS, and any factor peeled onto it. The
	// factor STAYS OUTSIDE the aggregate; all it does here is contribute its sign to
	// the easy/hard classification, and ride along on whichever form is emitted.
	Expression *lhs = UnwrapDecideCasts(*comp.left, decide.decide_index);
	ScaledAggregateMatch scale_match;
	const BoundAggregateExpression *scaled_agg =
	    TryMatchScaledAggregate(*lhs, decide.decide_index, scale_match) ? scale_match.aggregate : nullptr;
	auto scale = scale_match.scale;
	bool scale_divides = scale_match.divides;

	if (!scaled_agg && lhs->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return;
	}
	auto &agg = scaled_agg ? *scaled_agg : lhs->Cast<BoundAggregateExpression>();
	auto fname = StringUtil::Lower(agg.function.name);
	if (fname != "min" && fname != "max") {
		return;
	}
	if (agg.children.size() != 1) {
		return;
	}
	// Guard: only rewrite MIN/MAX over decide variables
	if (!BoundExpressionReferencesDecide(*agg.children[0], decide.decide_index)) {
		return;
	}
	// A factor that is itself decision-bearing is bilinear, not a scale; the
	// canonicalizer rejects those, so reaching one here would be a bug rather than a
	// user error. Leave the shape alone rather than mis-linearize it.
	if (scale && BoundExpressionReferencesDecide(*scale, decide.decide_index)) {
		return;
	}

	bool is_max = (fname == "max");
	auto cmp_type = comp.type;

	// A negative factor reverses the relation the aggregate faces: `-2 * MAX(e) >= -8`
	// is `MAX(e) <= 4`, the cheap direction, not the expensive one. Classify against
	// the flipped relation rather than the written one.
	//
	// An unknown sign (a scalar subquery factor) makes the cheap direction
	// undecidable, so take the expensive one -- it pins the auxiliary to the true
	// MIN/MAX in both directions and is therefore right for either sign.
	int scale_sign = ScaleSignAtPlanTime(optimizer.context, scale, scale_divides);

	// Neither of this path's two encodings is safe without the sign. "Easy" fans the
	// bound out over EVERY row; "hard" asserts SOME row attains it. Those are opposite
	// quantifiers, and a negative factor swaps which one the constraint means, so a
	// factor whose value is not known until the query runs cannot pick between them.
	// RewriteComposedMinMax claims that shape first (it runs earlier and emits both
	// halves); reaching here means it declined, so decline too rather than guess.
	if (scale_sign == 0) {
		return;
	}
	auto effective_cmp = (scale_sign < 0) ? FlipComparisonExpression(cmp_type) : cmp_type;

	// Classify: easy vs hard
	bool is_easy = false;
	if (is_max && (effective_cmp == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	               effective_cmp == ExpressionType::COMPARE_LESSTHAN)) {
		is_easy = true; // MAX(expr) <= K → every row: expr <= K
	}
	if (!is_max && (effective_cmp == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	                effective_cmp == ExpressionType::COMPARE_GREATERTHAN)) {
		is_easy = true; // MIN(expr) >= K → every row: expr >= K
	}

	bool is_hard = false;
	if (is_max && (effective_cmp == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	               effective_cmp == ExpressionType::COMPARE_GREATERTHAN)) {
		is_hard = true; // MAX(expr) >= K → need indicator
	}
	if (!is_max && (effective_cmp == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	                effective_cmp == ExpressionType::COMPARE_LESSTHAN)) {
		is_hard = true; // MIN(expr) <= K → need indicator
	}

	if (cmp_type == ExpressionType::COMPARE_NOTEQUAL) {
		throw BinderException("DECIDE does not support <> comparison with MIN/MAX aggregates.");
	}

	// Re-attach the peeled factor to whatever form is emitted below. `scale * e` keeps
	// the factor on the left, matching the one spelling canonicalization produces.
	auto apply_scale = [&](unique_ptr<Expression> inner) -> unique_ptr<Expression> {
		if (!scale) {
			return inner;
		}
		return scale_divides ? optimizer.BindScalarFunction("/", std::move(inner), scale->Copy())
		                     : optimizer.BindScalarFunction("*", scale->Copy(), std::move(inner));
	};

	if (cmp_type == ExpressionType::COMPARE_EQUAL) {
		// Equality: split into easy + hard parts
		// MAX(expr) = K → (expr <= K) AND (MAX(expr) >= K)
		// MIN(expr) = K → (expr >= K) AND (MIN(expr) <= K)

		// Easy part: per-row bound. A negative factor reverses it, for the same reason
		// it reverses the classification above.
		auto easy_cmp_type = is_max ? ExpressionType::COMPARE_LESSTHANOREQUALTO
		                            : ExpressionType::COMPARE_GREATERTHANOREQUALTO;
		if (scale_sign < 0) {
			easy_cmp_type = FlipComparisonExpression(easy_cmp_type);
		}
		unique_ptr<Expression> easy = make_uniq<BoundComparisonExpression>(
		    easy_cmp_type,
		    apply_scale(agg.children[0]->Copy()), comp.right->Copy());
		easy->alias = comp.alias;
		AddDecideTag(easy->alias, MINMAX_EASY_REWRITE_TAG);
		// Preserve aggregate-local WHEN filter as a per-row WHEN wrapper
		if (agg.filter) {
			auto when_wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
			when_wrapper->children.push_back(std::move(easy));
			when_wrapper->children.push_back(agg.filter->Copy());
			when_wrapper->alias = WHEN_CONSTRAINT_TAG;
			easy = std::move(when_wrapper);
		}
		new_constraints.push_back(std::move(easy));

		// Hard part: allocate indicator + tagged SUM
		auto hard_cmp_type = is_max ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
		                            : ExpressionType::COMPARE_LESSTHANOREQUALTO;
		if (scale_sign < 0) {
			hard_cmp_type = FlipComparisonExpression(hard_cmp_type);
		}
		idx_t ind_idx;
		auto hard_lhs = EmitHardMinMaxClause(decide, fname, *agg.children[0], agg.filter.get(), ind_idx);
		comp.left = apply_scale(std::move(hard_lhs));
		comp.type = hard_cmp_type;
		return;
	}

	if (is_easy) {
		// Easy case: strip the aggregate, make it per-row
		// MAX(expr) <= K → expr <= K
		// MIN(expr) >= K → expr >= K
		// Save filter before destroying the aggregate (comp.left assignment invalidates agg reference)
		unique_ptr<Expression> saved_filter;
		if (agg.filter) {
			saved_filter = agg.filter->Copy();
		}
		// The factor distributes over the per-row form and the relation keeps the
		// direction the user wrote: `s * MAX(e) <op> K` is `s*e_i <op> K` for every
		// row, for EITHER sign of s. (A negative s flips the relation twice -- once
		// getting from the aggregate to `MAX(e) <op'> K/s`, once dividing by s -- so
		// the two cancel. The classification above is what consumed the sign.)
		comp.left = apply_scale(agg.children[0]->Copy());
		// Tag the comparison so physical_decide.cpp can enforce empty-WHEN
		// rejection on constraints the user wrote as MIN/MAX, even after the
		// optimizer strips the aggregate.
		AddDecideTag(comp.alias, MINMAX_EASY_REWRITE_TAG);
		// Preserve aggregate-local WHEN filter as a per-row WHEN wrapper
		if (saved_filter) {
			auto when_wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
			when_wrapper->children.push_back(std::move(expr));
			when_wrapper->children.push_back(std::move(saved_filter));
			when_wrapper->alias = WHEN_CONSTRAINT_TAG;
			expr = std::move(when_wrapper);
		}
		out_was_easy = true;
		return;
	}

	if (is_hard) {
		// Hard case: allocate indicator + tagged SUM via shared helper. The factor
		// rides on the outside; the physical extractor multiplies it into the row's
		// coefficients, which is exact whatever its sign because the indicator layer
		// has already pinned the auxiliary to the true MIN/MAX.
		idx_t ind_idx;
		auto hard_lhs = EmitHardMinMaxClause(decide, fname, *agg.children[0], agg.filter.get(), ind_idx);
		comp.left = apply_scale(std::move(hard_lhs));
		return;
	}
}

unique_ptr<Expression> DecideOptimizer::EmitHardMinMaxClause(LogicalDecide &decide,
                                                            const string &agg_name,
                                                            const Expression &inner,
                                                            const Expression *filter,
                                                            idx_t &out_clause_idx) {
	// No indicator variable. `MAX(e) >= K` now becomes an extremum COLUMN and the user's
	// own bound as a single row over it, whichever way that column is then pinned, and the
	// binaries a Big-M pinning needs are global-block columns stage 06 allocates for the
	// rows it actually emits. A row-scoped binary per data row, created here before anyone
	// knows whether it will be read, is exactly what that removed.
	//
	// What stage 05 still owns is the marking: which clause this is, what it reduces with,
	// and the text to call it in a diagnosis. The clause index rides the tag.
	idx_t clause_idx = decide.minmax_clause_labels.size();
	decide.minmax_clause_labels.push_back(StringUtil::Upper(agg_name) + "(" + inner.ToString() + ")");

	// Build a SUM(inner) aggregate tagged as a hard MIN/MAX. The aggregate name is the
	// marking; the clause index rides along so stage 06 can name what it emits.
	vector<unique_ptr<Expression>> sum_children;
	sum_children.push_back(inner.Copy());
	auto new_sum = optimizer.BindAggregateFunction("sum", std::move(sum_children));
	if (filter) {
		new_sum->Cast<BoundAggregateExpression>().filter = filter->Copy();
	}
	new_sum->alias =
	    string(MINMAX_CLAUSE_TAG_PREFIX) + to_string(clause_idx) + "_" + agg_name + "__";
	out_clause_idx = clause_idx;
	return new_sum;
}

void DecideOptimizer::RewriteMinMaxObjective(LogicalDecide &decide) {
	if (!decide.decide_objective) {
		return;
	}
	// Detach, rewrite, reinstall through the boundary. The rewrite itself is
	// unchanged; it just operates on a local tree now, so whatever it produces is
	// re-canonicalized exactly like an optimizer-generated constraint is.
	auto objective = std::move(decide.decide_objective);
	RewriteMinMaxObjectiveTree(decide, objective);
	decide.SetObjective(optimizer.context, std::move(objective));
}

void DecideOptimizer::RewriteMinMaxObjectiveTree(LogicalDecide &decide, unique_ptr<Expression> &objective) {
	// Navigate through PER and WHEN wrappers to find the actual aggregate
	unique_ptr<Expression> *obj_owner = &objective;
	Expression *obj_expr = objective.get();
	bool has_per = false;

	// Unwrap PER wrapper (outermost layer)
	if (obj_expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = obj_expr->Cast<BoundConjunctionExpression>();
		if (IsPerConstraintTag(conj.alias) && !conj.children.empty()) {
			has_per = true;
			obj_owner = &conj.children[0];
			obj_expr = conj.children[0].get();
		}
	}

	// Unwrap WHEN wrapper (inside PER, if present)
	if (obj_expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = obj_expr->Cast<BoundConjunctionExpression>();
		if (HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG) && !conj.children.empty()) {
			obj_owner = &conj.children[0];
			obj_expr = conj.children[0].get();
		}
	}

	// Unwrap any BoundCastExpression
	if (obj_expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = obj_expr->Cast<BoundCastExpression>();
		obj_owner = &cast.child;
		obj_expr = cast.child.get();
	}

	// A factor peeled onto the objective's reducer (`2 * MAX(x*v)`, `MAX(x*v) * 2`).
	// Objectives are not canonicalized, so both spellings arrive as written.
	ScaledAggregateMatch scale_match;
	const BoundAggregateExpression *scaled_obj_agg =
	    TryMatchScaledAggregate(*obj_expr, decide.decide_index, scale_match) ? scale_match.aggregate : nullptr;
	auto obj_scale = scale_match.scale;

	// Now inspect the actual aggregate
	if (!scaled_obj_agg && obj_expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return;
	}
	auto &outer_agg = scaled_obj_agg ? const_cast<BoundAggregateExpression &>(*scaled_obj_agg)
	                                : obj_expr->Cast<BoundAggregateExpression>();
	auto outer_name = StringUtil::Lower(outer_agg.function.name);
	// A decision-bearing factor is bilinear, not a scale; the canonicalizer rejects
	// those, so leave the shape alone rather than mis-linearize it.
	if (obj_scale && BoundExpressionReferencesDecide(*obj_scale, decide.decide_index)) {
		return;
	}

	// Check for nested aggregate: OUTER(INNER(expr)) where INNER is also SUM/MIN/MAX/AVG
	if (has_per && (outer_name == "sum" || outer_name == "min" || outer_name == "max" || outer_name == "avg") &&
	    outer_agg.children.size() == 1) {
		// Unwrap cast on inner child if present
		Expression *inner_expr = outer_agg.children[0].get();
		if (inner_expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			inner_expr = inner_expr->Cast<BoundCastExpression>().child.get();
		}
		if (inner_expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			auto &inner_agg = inner_expr->Cast<BoundAggregateExpression>();
			auto inner_name = StringUtil::Lower(inner_agg.function.name);

			if ((inner_name == "sum" || inner_name == "min" || inner_name == "max" || inner_name == "avg") &&
			    inner_agg.children.size() == 1 &&
			    BoundExpressionReferencesDecide(*inner_agg.children[0], decide.decide_index)) {
				// Found nested pattern: set metadata
				// Map outer AVG → SUM (dividing by constant G doesn't change optimal)
				decide.per_outer_agg = (outer_name == "avg") ? ObjectiveAggregateType::SUM
				                                             : StrToAggType(outer_name);
				// Map inner AVG → SUM with flag for coefficient scaling
				if (inner_name == "avg") {
					decide.per_inner_agg = ObjectiveAggregateType::SUM;
					decide.per_inner_was_avg = true;
				} else {
					decide.per_inner_agg = StrToAggType(inner_name);
				}

				// Pre-compute easy/hard classification for inner and outer levels
				if (inner_name == "min" || inner_name == "max") {
					bool inner_is_min = (inner_name == "min");
					decide.per_inner_is_easy = (inner_is_min && decide.decide_sense == DecideSense::MAXIMIZE) ||
					                           (!inner_is_min && decide.decide_sense == DecideSense::MINIMIZE);
				}
				if (outer_name == "min" || outer_name == "max") {
					bool outer_is_min = (outer_name == "min");
					decide.per_outer_is_easy = (outer_is_min && decide.decide_sense == DecideSense::MAXIMIZE) ||
					                           (!outer_is_min && decide.decide_sense == DecideSense::MINIMIZE);
				}

				// Rewrite inner MIN/MAX/AVG → SUM for normalization
				if (inner_name == "min" || inner_name == "max" || inner_name == "avg") {
					vector<unique_ptr<Expression>> sum_children;
					sum_children.push_back(inner_agg.children[0]->Copy());
					auto new_sum = optimizer.BindAggregateFunction("sum", std::move(sum_children));
					if (inner_agg.filter) {
						new_sum->Cast<BoundAggregateExpression>().filter = inner_agg.filter->Copy();
					}
					// Replace inner aggregate within the outer
					outer_agg.children[0] = std::move(new_sum);
				}
				// Strip outer wrapper: replace OUTER(INNER(expr)) with INNER(expr)
				*obj_owner = std::move(outer_agg.children[0]);
				return;
			}
		}
	}

	// Flat MIN/MAX + PER → error (ambiguous without outer aggregate)
	if (has_per && (outer_name == "min" || outer_name == "max") &&
	    outer_agg.children.size() == 1 &&
	    BoundExpressionReferencesDecide(*outer_agg.children[0], decide.decide_index)) {
		throw BinderException(
		    "MINIMIZE/MAXIMIZE %s(...) PER is ambiguous. "
		    "With PER, use a nested aggregate to specify how per-group values are combined: "
		    "e.g., SUM(%s(...)) PER col or MAX(%s(...)) PER col.",
		    StringUtil::Upper(outer_name), StringUtil::Upper(outer_name),
		    StringUtil::Upper(outer_name));
	}

	// Flat non-PER MIN/MAX objective.
	//
	// A FACTOR on it never reaches here: this path replaces the whole objective with
	// its auxiliary at coefficient 1.0, so there is nowhere to put one.
	// RewriteComposedMinMaxObjectiveTop claims the scaled case first (it runs earlier
	// and carries a per-term scale), leaving this path the unscaled shape it was
	// written for and its cheaper encoding.
	if (obj_scale) {
		return;
	}
	if (!has_per && (outer_name == "min" || outer_name == "max") &&
	    outer_agg.children.size() == 1 &&
	    BoundExpressionReferencesDecide(*outer_agg.children[0], decide.decide_index)) {
		decide.flat_objective_agg = StrToAggType(outer_name);
		bool is_min = (outer_name == "min");
		decide.flat_objective_is_easy = (is_min && decide.decide_sense == DecideSense::MAXIMIZE) ||
		                                (!is_min && decide.decide_sense == DecideSense::MINIMIZE);
		// Replace MIN/MAX with SUM
		vector<unique_ptr<Expression>> sum_children;
		sum_children.push_back(outer_agg.children[0]->Copy());
		auto new_sum = optimizer.BindAggregateFunction("sum", std::move(sum_children));
		if (outer_agg.filter) {
			new_sum->Cast<BoundAggregateExpression>().filter = outer_agg.filter->Copy();
		}
		*obj_owner = std::move(new_sum);
	}
}

} // namespace duckdb
