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
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

// ---------------------------------------------------------------------------
// A factor sitting on a reducer
// ---------------------------------------------------------------------------
//
// The canonicalizer peels a factor OUTWARD off a reducer (`2 * SUM(x*p)`) and
// converges every spelling onto one. It stays outside from there: nothing pushes
// it back into the reducer's body.
//
// WHY IT STAYS OUTSIDE. `MIN`/`MAX` are order statistics, not linear functionals.
// They commute with a POSITIVE factor only -- `MAX(-2x)` is `-2*MIN(x)`, not
// `-2*MAX(x)` -- so pushing a factor in requires knowing its sign, and a scalar
// subquery's sign is not known until the query runs. Leaving the factor outside
// makes the sign irrelevant to CORRECTNESS: it only selects which linearization is
// cheaper. An unknown sign then costs performance instead of failing.
//
// (A previous version of this pass did fold, swapping MIN/MAX for a negative
// factor. It was exact, but it made the sign load-bearing for correctness and so
// had nothing to fall back on when the sign was unknown.)
//
// Where the factor is finally applied depends on the term:
//   SUM/AVG   -- multiplied into the per-row coefficients
//               (`PhysicalDecide::ApplyScaleToExtracted`)
//   MIN/MAX   -- multiplied into the auxiliary's contribution
//               (`ComposedMinMaxTerm::scale`), or distributed over the per-row
//               form when the constraint linearizes the easy way.

//! What the plan says a MIN/MAX column became: held by its envelope in the cheap direction; otherwise pinned by
//! indicator rows, or stated natively when its members are defined bodies (BODY_DEFINITION_TAG), which have no box.
static LogicalDecide::RewriteAccount::Kind ExtremumAccount(bool is_easy, const BoundAggregateExpression &agg) {
	using Kind = LogicalDecide::RewriteAccount::Kind;
	return is_easy                                        ? Kind::EXTREMUM
	       : HasDecideTag(agg.alias, BODY_DEFINITION_TAG) ? Kind::EXTREMUM_NATIVE
	                                                      : Kind::EXTREMUM_PINNED;
}

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
// Throws BinderException on the shapes the composed form does not take (non-aggregate
// leaves, nested subtraction/scaling, non-SUM/AVG/MIN/MAX aggregates).
//
// `outer_push` is which way the outer comparison or objective drives the LHS, and decides
// each MIN/MAX term's easy/hard classification; NONE (an outer `=`) classifies every
// MIN/MAX term hard, with its auxiliary pinned from both sides.
//! `names` quotes a refused term as written. `beside`, given for the objective, takes each term that is not a MIN or
//! MAX (`sum(x)` and `s` in `max(x) + sum(x) + s`) with its sign: a constraint's terms share the row its MIN/MAX
//! auxiliaries stand in, but an objective has no such row, so its other terms stay on the objective's own path.
static void WalkComposedLhs(ClientContext &context, LogicalDecide &decide, idx_t source_clause_id, const Expression &e,
                            int sign, idx_t decide_index, ExtremumPush outer_push,
                            vector<LogicalDecide::ComposedMinMaxTerm> &out_terms, const DecideSourceNames &names,
                            optional_ptr<vector<pair<int, const Expression *>>> beside = nullptr) {
	auto &u = (*UnwrapDecideCasts(e, decide_index));
	if (IsAddNode(u, decide_index)) {
		auto &fn = u.Cast<BoundFunctionExpression>();
		for (auto &child : fn.children) {
			WalkComposedLhs(context, decide, source_clause_id, *child, sign, decide_index, outer_push, out_terms, names,
			                beside);
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
			WalkComposedLhs(context, decide, source_clause_id, *fn.children[0], sign, decide_index, outer_push,
			                out_terms, names, beside);
			WalkComposedLhs(context, decide, source_clause_id, *fn.children[1], -sign, decide_index, outer_push,
			                out_terms, names, beside);
			return;
		}
		if (fn.children.size() == 1) {
			WalkComposedLhs(context, decide, source_clause_id, *fn.children[0], -sign, decide_index, outer_push,
			                out_terms, names, beside);
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

	const BoundAggregateExpression *found = scaled_agg ? scaled_agg
	                                        : u.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE
	                                            ? &u.Cast<BoundAggregateExpression>()
	                                            : nullptr;
	auto name = found ? StringUtil::Lower(found->function.name) : string();
	if (beside && name != "min" && name != "max") {
		beside->emplace_back(sign, &e);
		return;
	}
	if (!found) {
		throw BinderException("DECIDE: '%s' beside MIN or MAX is not supported yet", RenderDecideSource(e, names));
	}
	auto &agg = *found;
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
		// An outer `=` holds the LHS from both sides, and a factor whose sign is not known
		// until the query runs leaves the direction open (ExtremumPush::NONE either way):
		// neither has a cheap direction, so the indicator layer pins z to the true MIN/MAX
		// from both sides -- right for an equality, and right for either sign.
		// A factor that is exactly zero makes the term vanish: nothing pushes z, so the
		// cheap envelope is right whichever way the clause faces, and it asks for no
		// member box (`sum(x) + 0 * min(abs(x - 2)) <= 5` was refused for one).
		int scale_sign = ScaleSignAtPlanTime(context, scale, scale_divides);
		term.is_easy = ScaleVanishesAtPlanTime(context, scale, scale_divides) ||
		               IsEasyExtremum(is_max, ExtremumPushOf(outer_push, sign * scale_sign));
		AddRewriteAccount(decide, source_clause_id, ExtremumAccount(term.is_easy, agg), RenderForPlan(agg, decide));
	}
	out_terms.push_back(std::move(term));
}

//! A composed clause reads only the rows its `when` keeps: the condition joins each
//! term's own `when`. A non-empty `by` would group the clause, which composed rows are
//! not.
static void ScopeComposedTerms(vector<LogicalDecide::ComposedMinMaxTerm> &terms, const Expression *when) {
	for (auto &term : terms) {
		if (!when) {
			continue;
		}
		if (!term.filter) {
			term.filter = when->Copy();
			continue;
		}
		auto both = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		both->children.push_back(when->Copy());
		both->children.push_back(std::move(term.filter));
		term.filter = std::move(both);
	}
}

//! The composed formulation is one row over all of a clause's kept rows, and its bound
//! is one number: the canonicalizer sends a clause with a `by` or another bound to the
//! value rewrite instead (DecideCanonicalizer::ClassifyScoped).
static void AssertComposable(const BoundComparisonExpression &comparison) {
	idx_t group_key;
	if (TryParseGroupKeyTag(comparison.GetAlias(), group_key) || !comparison.right->IsFoldable()) {
		throw InternalException("DECIDE: a composed MIN/MAX clause reached the rewrite with a by key or a "
		                        "bound that is not constant: %s",
		                        comparison.ToString());
	}
}

void DecideOptimizer::RewriteComposedMinMaxInConstraint(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                                        DecideEnclosingScope scope) {
	if (!expr) {
		return;
	}

	// Walk through AND conjunctions and the `when` / `per K:` wrappers. Inside `when c:` a
	// composed clause's terms read the rows `c` keeps; a `per K:` over a composed clause
	// names the classes its one row stands for (the checks hold K to it) and adds no row,
	// so it keeps the placeholder the rewrite leaves.
	scope = EnterDecideScope(*expr, scope);
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr->Cast<BoundConjunctionExpression>();
		for (idx_t i = 0; i < conj.children.size(); i++) {
			if (IsConstraintChild(conj, i)) {
				RewriteComposedMinMaxInConstraint(conj.children[i], decide, scope);
			}
		}
		// The `when c:` folds into the terms, which then read only the rows it keeps; the
		// wrapper goes with the clause it held.
		if (IsWhenFilterWrapper(conj) && conj.children.size() == 2) {
			auto &inner = *conj.children[0];
			if (inner.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
			    inner.return_type == LogicalType::BOOLEAN) {
				expr = std::move(conj.children[0]); // the placeholder of a composed clause
			}
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
	// An equality has no cheap direction: it holds the LHS from both sides, so every
	// MIN/MAX term needs its auxiliary pinned exactly rather than pushed one way. That
	// costs the indicator layer on every term, but it is still cheaper than the
	// `BETWEEN K AND K` spelling users reached for while `=` was rejected -- two
	// comparisons allocate two auxiliaries and pay for the indicator layer on one of
	// them anyway, plus a redundant second envelope.
	bool exact_pin = (cmp_type == ExpressionType::COMPARE_EQUAL);
	if (!exact_pin && cmp_type != ExpressionType::COMPARE_LESSTHAN &&
	    cmp_type != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
	    cmp_type != ExpressionType::COMPARE_GREATERTHAN &&
	    cmp_type != ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
		// BETWEEN never reaches here — it is already a pair of directional
		// comparisons by this point — so the message must not claim otherwise.
		throw BinderException("Composed MIN/MAX in DECIDE v1 supports only the <, <=, =, >, >= and "
		                      "BETWEEN comparisons.");
	}

	const ExtremumPush outer_push = ExtremumPushOf(cmp_type);

	auto names = DecidePlanNames(decide.source_fragments, decide.keyed_scopes, decide.keys, decide.decide_index,
	                             decide.aux_var_expressions);
	AssertComposable(comp);
	LogicalDecide::ComposedMinMaxConstraint spec;
	spec.outer_cmp = cmp_type;
	spec.rhs_expr = comp.right->Copy();
	TryParseSourceClauseTag(comp.GetAlias(), spec.source_clause_id);
	TryParseRemovalGroupTag(comp.GetAlias(), spec.removal_group_id);

	WalkComposedLhs(optimizer.context, decide, spec.source_clause_id, *comp.left, /*sign=*/1, decide.decide_index,
	                outer_push, spec.terms, names);
	ScopeComposedTerms(spec.terms, scope.Condition());
	// The classes its one row stands for (D28): its `per` key's values, or with no `per`
	// every row its `when` keeps.
	spec.scope = DecideClauseScope(scope);

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

	// `MAXIMIZE when c:` folds into the terms. The two-level form's PER wrapper never holds
	// a composed body (its child is `outer(inner(e))`), so it declines below as a
	// conjunction.
	const Expression *body = &obj;
	const Expression *scoped_when = nullptr;
	if (obj.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = obj.Cast<BoundConjunctionExpression>();
		if (IsWhenFilterWrapper(conj) && conj.children.size() == 2 &&
		    AdditiveContainsMinMax(*conj.children[0], decide.decide_index)) {
			body = conj.children[0].get();
			scoped_when = conj.children[1].get();
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
	    TryMatchScaledAggregate(*body, decide.decide_index, lone_match) ? lone_match.aggregate : nullptr;
	auto lone_scale = lone_match.scale;
	bool is_scaled_lone_minmax =
	    lone_scaled && lone_scale &&
	    (StringUtil::Lower(lone_scaled->function.name) == "min" ||
	     StringUtil::Lower(lone_scaled->function.name) == "max") &&
	    lone_scaled->children.size() == 1 &&
	    BoundExpressionReferencesDecide(*lone_scaled->children[0], decide.decide_index) &&
	    !BoundExpressionReferencesDecide(*lone_scale, decide.decide_index);

	if (!is_scaled_lone_minmax) {
		if (!IsAddNode(*body, decide.decide_index) && !IsSubNode(*body, decide.decide_index)) {
			return;
		}
		if (!AdditiveContainsMinMax(*body, decide.decide_index)) {
			return;
		}
	}

	// Direction: MAXIMIZE pushes each term UP; MINIMIZE pushes each term DOWN.
	const ExtremumPush outer_push =
	    decide.decide_sense == DecideSense::MINIMIZE ? ExtremumPush::DOWN : ExtremumPush::UP;

	vector<LogicalDecide::ComposedMinMaxTerm> terms;
	vector<pair<int, const Expression *>> beside;
	// An objective always has a direction, so no term needs the both-sides pin here.
	auto names = DecidePlanNames(decide.source_fragments, decide.keyed_scopes, decide.keys, decide.decide_index,
	                             decide.aux_var_expressions);
	WalkComposedLhs(optimizer.context, decide, DConstants::INVALID_INDEX, *body, /*sign=*/1, decide.decide_index,
	                outer_push, terms, names, beside);
	ScopeComposedTerms(terms, scoped_when);

	decide.composed_minmax_objective_terms = std::move(terms);

	// The objective keeps the terms beside its MIN/MAX terms, under its `when`, and the physical layer adds each
	// auxiliary's coefficient to theirs; with none it is a zero placeholder.
	unique_ptr<Expression> rest;
	for (auto &term : beside) {
		auto copy = term.second->Copy();
		if (!rest) {
			rest = term.first > 0 ? std::move(copy) : optimizer.BindScalarFunction("-", std::move(copy));
		} else {
			rest = optimizer.BindScalarFunction(term.first > 0 ? "+" : "-", std::move(rest), std::move(copy));
		}
	}
	if (!rest) {
		rest = make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0));
	} else if (scoped_when) {
		auto &conj = obj.Cast<BoundConjunctionExpression>();
		auto wrapper = make_uniq<BoundConjunctionExpression>(conj.type);
		wrapper->children.push_back(std::move(rest));
		wrapper->children.push_back(scoped_when->Copy());
		wrapper->SetAlias(conj.GetAlias());
		rest = std::move(wrapper);
	}
	decide.SetObjective(optimizer.context, std::move(rest));
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
	RewriteMinMaxInConstraint(decide.decide_constraints, decide, new_constraints);

	// Append generated constraints (from equality splitting) to the constraint tree
	for (auto &nc : new_constraints) {
		AppendConstraint(decide, std::move(nc));
	}
}

void DecideOptimizer::RewriteMinMaxInConstraint(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                                vector<unique_ptr<Expression>> &new_constraints,
                                                DecideEnclosingScope scope) {
	if (!expr) {
		return;
	}

	// The `when` and `per K:` travel with the clause they hold, so a row this rewrite adds
	// for the clause keeps the clause's rows and classes. A wrapper's key or condition
	// children are data (decide_constraint_walk.hpp).
	scope = EnterDecideScope(*expr, scope);
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr->Cast<BoundConjunctionExpression>();
		for (idx_t i = 0; i < conj.children.size(); i++) {
			if (IsConstraintChild(conj, i)) {
				RewriteMinMaxInConstraint(conj.children[i], decide, new_constraints, scope);
			}
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
	// Classify: easy (`MAX(expr) <= K` is every row's `expr <= K`; `MIN(expr) >= K` the
	// mirror) against hard (`MAX(expr) >= K` needs an indicator: some row reaches K).
	// `=` is neither and is split below.
	const ExtremumPush push = ExtremumPushOf(ExtremumPushOf(cmp_type), scale_sign);
	const bool is_easy = IsEasyExtremum(is_max, push);
	const bool is_hard = push != ExtremumPush::NONE && !is_easy;

	if (cmp_type == ExpressionType::COMPARE_NOTEQUAL) {
		// Name the clause as written: a `norm(e, 'inf')` reaches here lowered onto a MAX.
		throw BinderException("DECIDE does not support <> comparison with MIN/MAX aggregates (norm(e, 'inf') is a MAX "
		                      "of absolute values): %s",
		                      QuoteWrittenClause(decide.constraint_sources[SourceClauseOf(comp, decide)]));
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

	// A clause keeps its reducer through the easy direction: its `when` picks the members
	// its rows are emitted for, while its bound is reduced over every kept row of the `by`
	// group (the linear form expands it). Hoisting the `when` onto the clause would reduce
	// the bound over the members only.
	// The hard reducer the indicators pin is rebuilt as a SUM; it keeps its `by`.
	auto keep_group_key = [&](unique_ptr<Expression> hard_lhs) {
		idx_t group_key;
		if (TryParseGroupKeyTag(agg.alias, group_key)) {
			AddDecideTag(hard_lhs->alias, MakeGroupKeyTag(group_key));
		}
		return hard_lhs;
	};
	// The hard side. Stage 06 takes the extremum the tag names over the members'
	// coefficients -- the factor included -- and states the clause's own relation over
	// it, which is the clause only when the factor is inside the reducer: with a negative
	// factor `s * MAX(e)` is `MIN(s * e)`.
	auto build_hard_lhs = [&](idx_t &ind_idx) -> unique_ptr<Expression> {
		if (scale_sign > 0) {
			return apply_scale(
			    keep_group_key(EmitHardMinMaxClause(decide, fname, *agg.children[0], agg.filter.get(), ind_idx)));
		}
		auto scaled_body = apply_scale(agg.children[0]->Copy());
		return keep_group_key(
		    EmitHardMinMaxClause(decide, is_max ? "min" : "max", *scaled_body, agg.filter.get(), ind_idx));
	};

	// What the plan says the reducer became: an extremum column for the hard direction, a
	// bound on each member for the cheap one, both for an equality.
	if (is_hard || cmp_type == ExpressionType::COMPARE_EQUAL) {
		AddRewriteAccount(decide, comp.alias, ExtremumAccount(false, agg), RenderForPlan(agg, decide));
	}
	if (is_easy || cmp_type == ExpressionType::COMPARE_EQUAL) {
		AddRewriteAccount(decide, comp.alias, LogicalDecide::RewriteAccount::Kind::EXTREMUM_EACH_MEMBER,
		                  RenderForPlan(agg, decide));
	}

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
		// The easy half keeps the reducer (MINMAX_EASY_REWRITE_TAG): the linear form
		// expands it to one row per member.
		unique_ptr<Expression> easy =
		    make_uniq<BoundComparisonExpression>(easy_cmp_type, comp.left->Copy(), comp.right->Copy());
		easy->alias = comp.alias;
		AddDecideTag(easy->alias, MINMAX_EASY_REWRITE_TAG);
		easy = ScopeHelperRow(std::move(easy), scope.Condition());
		if (scope.per_wrapper) {
			auto per_class = scope.per_wrapper;
			// The clause's own `per K:`, outermost as the binder nests it: the easy half
			// stands for the same classes (D28) and prints in front as written.
			auto per_wrapper = make_uniq<BoundConjunctionExpression>(per_class->type);
			per_wrapper->alias = per_class->alias;
			per_wrapper->children.push_back(std::move(easy));
			for (idx_t i = 1; i < per_class->children.size(); i++) {
				per_wrapper->children.push_back(per_class->children[i]->Copy());
			}
			easy = std::move(per_wrapper);
		}
		new_constraints.push_back(std::move(easy));
		auto hard_cmp_type = is_max ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
		                            : ExpressionType::COMPARE_LESSTHANOREQUALTO;
		if (scale_sign < 0) {
			hard_cmp_type = FlipComparisonExpression(hard_cmp_type);
		}
		idx_t ind_idx;
		comp.left = build_hard_lhs(ind_idx);
		comp.type = hard_cmp_type;
		return;
	}

	if (is_easy) {
		// The reducer stays: the linear form bounds each member (its `when` chooses the
		// members, its `by` the rows the bound is reduced over); bound absorption folds a
		// bare `max(x) <= K` into the column box (D41).
		AddDecideTag(comp.alias, MINMAX_EASY_REWRITE_TAG);
		return;
	}

	if (is_hard) {
		idx_t ind_idx;
		comp.left = build_hard_lhs(ind_idx);
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
	// and the text to call it in a diagnosis, spelled as the plan prints the reducer. The
	// clause index rides the tag.
	idx_t clause_idx = decide.minmax_clause_labels.size();
	decide.minmax_clause_labels.push_back(agg_name + "(" + RenderForPlan(inner, decide) + ")");

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
	// Navigate through the two-level form's PER wrapper and the `when` wrapper to find the actual aggregate
	unique_ptr<Expression> *obj_owner = &objective;
	Expression *obj_expr = objective.get();
	bool has_per = false;
	const BoundConjunctionExpression *per_wrapper = nullptr;

	// Unwrap PER wrapper (outermost layer)
	if (obj_expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = obj_expr->Cast<BoundConjunctionExpression>();
		if (IsTwoLevelObjectiveWrapper(conj) && !conj.children.empty()) {
			has_per = true;
			per_wrapper = &conj;
			obj_owner = &conj.children[0];
			obj_expr = conj.children[0].get();
		}
	}

	// Unwrap WHEN wrapper (inside PER, if present)
	if (obj_expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = obj_expr->Cast<BoundConjunctionExpression>();
		if (IsWhenFilterWrapper(conj) && !conj.children.empty()) {
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
	// Objectives are not canonicalized, so either order arrives as written.
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
	if (has_per && (outer_name == "sum" || outer_name == "min" || outer_name == "max") &&
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
				// Found the two-level pattern: set metadata. (The binder refuses an outer
				// avg: BindTwoLevelObjective.)
				decide.per_outer_agg = StrToAggType(outer_name);
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
				// What the plan says the two levels became, read before the inner is rebuilt.
				// The `by` of the inner reducer is the `per` key, which the wrapper holds, so the
				// subjects spell it out as written.
				{
					using Kind = LogicalDecide::RewriteAccount::Kind;
					const string key =
					    SplitDecideTwoLevel(*per_wrapper, decide.source_fragments, decide.keyed_scopes).key;
					auto inner_text = RenderDecideTwoLevelInner(RenderForPlan(inner_agg, decide), key);
					// Defined members have no box, so every extremum over them, the outer one's
					// included, is stated natively.
					const bool defined = HasDecideTag(inner_agg.alias, BODY_DEFINITION_TAG);
					if (inner_name == "min" || inner_name == "max") {
						AddRewriteAccount(decide, DConstants::INVALID_INDEX,
						                  decide.per_inner_is_easy ? Kind::EXTREMUM_PER_GROUP
						                  : defined                ? Kind::EXTREMUM_PER_GROUP_NATIVE
						                                           : Kind::EXTREMUM_PER_GROUP_PINNED,
						                  inner_text);
					}
					if (inner_name == "avg") {
						AddRewriteAccount(decide, DConstants::INVALID_INDEX, Kind::AVG_SUM, inner_text);
					}
					if (outer_name == "min" || outer_name == "max") {
						AddRewriteAccount(decide, DConstants::INVALID_INDEX,
						                  decide.per_outer_is_easy ? Kind::EXTREMUM
						                  : defined                ? Kind::EXTREMUM_NATIVE
						                                           : Kind::EXTREMUM_PINNED,
						                  RenderDecideTwoLevelOuter(outer_name, key, inner_text));
					}
				}

				// Rewrite inner MIN/MAX/AVG → SUM for normalization
				if (inner_name == "min" || inner_name == "max" || inner_name == "avg") {
					vector<unique_ptr<Expression>> sum_children;
					sum_children.push_back(inner_agg.children[0]->Copy());
					auto new_sum = optimizer.BindAggregateFunction("sum", std::move(sum_children));
					if (inner_agg.filter) {
						new_sum->Cast<BoundAggregateExpression>().filter = inner_agg.filter->Copy();
					}
					// The inner reducer's tags (its `by` key) stay on the sum that stands in
					// for it, so a plan still prints it as written.
					new_sum->alias = inner_agg.alias;
					// Replace inner aggregate within the outer
					outer_agg.children[0] = std::move(new_sum);
				}
				// Strip outer wrapper: replace OUTER(INNER(expr)) with INNER(expr)
				*obj_owner = std::move(outer_agg.children[0]);
				return;
			}
		}
	}

	// Flat MIN/MAX objective (not the two-level form).
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
		AddRewriteAccount(decide, DConstants::INVALID_INDEX, ExtremumAccount(decide.flat_objective_is_easy, outer_agg),
		                  RenderForPlan(outer_agg, decide));
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
