//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/optimizer/decide/decide_rewrite_define.cpp
//
// DECIDE definition of a non-linear reducer body by helper decisions. See decide_optimizer.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"
#include "duckdb/optimizer/decide/decide_solver_gate.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

// ---------------------------------------------------------------------------
// Defining a non-linear reducer body (the Plaisted-Greenbaum encoding)
// ---------------------------------------------------------------------------
//
// Every extremum the later passes build reads its members as linear: the hard rows, the
// composed terms, the flat and the two-level objective. A body that squares or multiplies
// decisions (`max(power(x, 2))`, once RewriteBilinear has taken the Boolean products)
// would lose its square there. This pass names such a body first: one helper decision `t`
// per instance of the scopes the body reads (ScopeOf: per row once it reads a data column)
// and one quadratic row defining it by the body `q` per instance, so the reducer reads the
// bare column `t` and every later pass keeps reading linear members. RewriteAbs does the
// same for an ABS.
//
// The row is one-sided where the clause pushes the reducer one way, as the
// Plaisted-Greenbaum encoding states a definition only in the polarity it occurs in. SUM,
// AVG, MIN and MAX never decrease when a member grows, so a member is pushed the way its
// reducer is (WalkPolarity): pushed down, `t >= q`; pushed up, `t <= q`; neither (`=`, a
// factor whose sign only the rows decide), `t = q`. A convex `q` pushed down stays convex.
//
// One reducer over one set of rows is defined once (structural sharing): an earlier rewrite
// may have copied it into several rows of a clause (RewriteAbs's two envelope rows), and the
// copies must keep reading one value, so they share one helper whose row holds in every
// direction a copy is pushed. Clauses and the objective holding the same reducer share it the
// same way. The definition belongs to no clause (owner ruling B10, as for the value
// rewrite's helpers): a DROP of a reading clause leaves it, which is harmless, since a row
// on a helper nothing else reads always holds.
//
// Which bodies: every MIN or MAX's; in a constraint that holds a MIN or MAX, every SUM and
// AVG term's too (the composed row reads them as linear); in a constraint the value rewrite
// takes (VALUE shape), every SUM and AVG term's (the helper decision that holds the reducer
// is defined over linear members, and it is the same epigraph form); and the inner
// reducer's of a two-level objective under an outer MIN or MAX. A plain constraint whose one
// MIN or MAX is pushed its cheap way (`max(q) <= K`) is left alone: the MIN/MAX rewrite makes
// it one quadratic row per member and needs no helper. An objective's SUM beside a MIN or MAX
// stays on the objective's own quadratic path.
//
// A helper has no box, so the Big-M arm has no constant for an extremum over it: only a
// backend that states MIN/MAX natively takes one (stage 08 takes the native arm for a family
// with no box). Any other backend is refused here, before a row is read. A SUM or AVG that
// no MIN or MAX reads needs no such backend: its row is quadratic, which the model class
// gate (decide_solver_gate.cpp) refuses on a backend that states none.

namespace {

bool IsExtremumName(const string &name) {
	return name == "min" || name == "max";
}

//! A SUM, AVG, MIN or MAX over decisions.
optional_ptr<const BoundAggregateExpression> AsDecisionReducer(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return nullptr;
	}
	auto &agg = expr.Cast<BoundAggregateExpression>();
	auto name = StringUtil::Lower(agg.function.name);
	if ((name != "sum" && name != "avg" && !IsExtremumName(name)) || agg.children.size() != 1 ||
	    !BoundExpressionReferencesDecide(*agg.children[0], decide_index)) {
		return nullptr;
	}
	return &agg;
}

//! Does `expr` hold a reducer over decisions (with `extremum_only`, a MIN or MAX) at or below it?
bool HoldsReducer(const Expression &expr, idx_t decide_index, bool extremum_only) {
	auto reducer = AsDecisionReducer(expr, decide_index);
	if (reducer && (!extremum_only || IsExtremumName(StringUtil::Lower(reducer->function.name)))) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = HoldsReducer(child, decide_index, extremum_only);
		}
	});
	return found;
}

//! A reducer whose body is defined, the way its clause pushes it, and the clause it sits in.
struct BodySite {
	BoundAggregateExpression *reducer;
	ExtremumPush push;
	//! A SUM or AVG term beside a MIN or MAX, rather than one or a reducer inside one: what the refusal says.
	bool beside_extremum;
	//! Whether a MIN or MAX reads this body, so the backend must state MIN/MAX natively: the reducer is one, sits
	//! inside one, or sits in a constraint that holds one.
	bool needs_native_extremum;
	string source_alias;
	DecideEnclosingScope scope;
};

//! The sign every reducer in `side` carries (WalkPolarity), keyed by node.
void CollectReducerSigns(ClientContext &context, Expression &side, int sign, idx_t decide_index,
                         unordered_map<const Expression *, int> &signs) {
	PolarityHolds holds = [&](const Expression &node) {
		return HoldsReducer(node, decide_index, false);
	};
	PolarityVisit visit = [&](Expression &node, int node_sign) {
		if (AsDecisionReducer(node, decide_index)) {
			signs[&node] = node_sign;
		}
		return false;
	};
	WalkPolarity(context, side, sign, holds, visit);
}

//! The reducers under `expr` whose body is not linear and holds no reducer. A SUM or AVG qualifies with `sums`
//! (a constraint that holds a MIN or MAX) or `under_extremum` (the inner reducer of a two-level objective). A reducer
//! the polarity walk did not reach (`signs`) is pushed neither way.
void CollectBodySites(Expression &expr, idx_t decide_index, bool sums, bool extremum_clause, bool under_extremum,
                      ExtremumPush outer, const unordered_map<const Expression *, int> &signs,
                      const string &source_alias, const DecideEnclosingScope &scope, vector<BodySite> &sites) {
	if (!AsDecisionReducer(expr, decide_index)) {
		ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
			CollectBodySites(child, decide_index, sums, extremum_clause, under_extremum, outer, signs, source_alias,
			                 scope, sites);
		});
		return;
	}
	auto &reducer = expr.Cast<BoundAggregateExpression>();
	const bool is_extremum = IsExtremumName(StringUtil::Lower(reducer.function.name));
	auto &body = *reducer.children[0];
	if (HoldsReducer(body, decide_index, false)) {
		CollectBodySites(body, decide_index, sums, extremum_clause, is_extremum, outer, signs, source_alias, scope,
		                 sites);
		return;
	}
	if (DecideExpressionDegree(body, decide_index).IsLinear() || (!is_extremum && !sums && !under_extremum)) {
		return;
	}
	auto sign = signs.find(&expr);
	const auto push = sign == signs.end() ? ExtremumPush::NONE : ExtremumPushOf(outer, sign->second);
	sites.push_back({&reducer, push, !is_extremum && !under_extremum,
	                 is_extremum || under_extremum || extremum_clause, source_alias, scope});
}

//! A plain constraint whose one MIN or MAX is pushed its cheap way (`max(q) <= K`, `-2 * min(q) <= K`): the MIN/MAX
//! rewrite bounds each member, so a non-linear body is one quadratic row per member. Read as RewriteMinMaxInConstraint
//! reads it; a factor whose sign only the rows decide goes to the composed rewrite, which needs the definition.
bool IsCheapLoneExtremum(ClientContext &context, BoundComparisonExpression &comparison, idx_t decide_index) {
	auto &lhs = *UnwrapDecideCasts(*comparison.left, decide_index);
	ScaledAggregateMatch match;
	const BoundAggregateExpression *agg = TryMatchScaledAggregate(lhs, decide_index, match) ? match.aggregate
	                                      : lhs.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE
	                                          ? &lhs.Cast<BoundAggregateExpression>()
	                                          : nullptr;
	if (!agg || !IsExtremumName(StringUtil::Lower(agg->function.name)) ||
	    (match.scale && BoundExpressionReferencesDecide(*match.scale, decide_index))) {
		return false;
	}
	const int scale_sign = ScaleSignAtPlanTime(context, match.scale, match.divides);
	return IsEasyExtremum(StringUtil::Lower(agg->function.name) == "max",
	                      ExtremumPushOf(ExtremumPushOf(comparison.type), scale_sign));
}

//! Copies of one reducer under one `when`: they share a helper.
bool SameReducer(const BodySite &a, const BodySite &b) {
	if (a.reducer->alias != b.reducer->alias || !a.reducer->Equals(*b.reducer)) {
		return false;
	}
	auto a_when = a.scope.Condition();
	auto b_when = b.scope.Condition();
	if (!a_when || !b_when) {
		return !a_when && !b_when;
	}
	return a_when->Equals(*b_when);
}

//! The clause's `when` and the reducer's own filter: the rows a helper is defined on.
unique_ptr<Expression> HelperCondition(const BodySite &site) {
	auto when = site.scope.Condition();
	if (!when || !site.reducer->filter) {
		return when ? when->Copy() : site.reducer->filter ? site.reducer->filter->Copy() : nullptr;
	}
	return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, when->Copy(),
	                                             site.reducer->filter->Copy());
}

[[noreturn]] void RefuseWithoutNativeMinMax(const BodySite &site, LogicalDecide &decide) {
	const auto name = StringUtil::Lower(site.reducer->function.name);
	const string reducer_text = RenderForPlan(*site.reducer, decide);
	SolverModelClass needed;
	needed.quadratic_constraints = true;
	string cause = IsExtremumName(name)
	                   ? "'" + reducer_text + "' takes the " + (name == "max" ? "maximum" : "minimum") +
	                         " of a squared or multiplied expression"
	                   : "'" + reducer_text + "' squares or multiplies decision variables " +
	                         (site.beside_extremum ? "beside" : "inside") + " a MIN or MAX";
	ThrowDecideSolverGap(needed, cause,
	                     "replace " + RenderForPlan(*site.reducer->children[0], decide) +
	                         " with an expression linear in the decision variables");
}

} // namespace

void DecideOptimizer::DefineNonlinearReducerBodies(LogicalDecide &decide) {
	auto &context = optimizer.context;
	const idx_t decide_index = decide.decide_index;
	vector<BodySite> sites;

	if (decide.decide_constraints) {
		std::function<void(Expression &, const string &, DecideEnclosingScope)> walk = [&](Expression &expr,
		                                                                                   const string &inherited,
		                                                                                   DecideEnclosingScope scope) {
			auto source_alias = DescendSourceAlias(expr, inherited);
			scope = EnterDecideScope(expr, scope);
			if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
				auto &conj = expr.Cast<BoundConjunctionExpression>();
				for (idx_t i = 0; i < conj.children.size(); i++) {
					if (IsConstraintChild(conj, i)) {
						walk(*conj.children[i], source_alias, scope);
					}
				}
				return;
			}
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
				return;
			}
			// A MIN or MAX in the constraint, or any reducer the value rewrite holds in a helper decision.
			const bool extremum_clause = HoldsReducer(expr, decide_index, true);
			if (!extremum_clause && !(HasDecideTag(expr.GetAlias(), VALUE_REWRITE_TAG) &&
			                          HoldsReducer(expr, decide_index, false))) {
				return;
			}
			auto &comparison = expr.Cast<BoundComparisonExpression>();
			if (IsCheapLoneExtremum(context, comparison, decide_index)) {
				return;
			}
			// Read as `LHS - RHS <op> 0`: the relation pushes the whole expression.
			unordered_map<const Expression *, int> signs;
			CollectReducerSigns(context, *comparison.left, 1, decide_index, signs);
			CollectReducerSigns(context, *comparison.right, -1, decide_index, signs);
			const auto outer = ExtremumPushOf(comparison.type);
			CollectBodySites(*comparison.left, decide_index, true, extremum_clause, false, outer, signs, source_alias,
			                 scope, sites);
			CollectBodySites(*comparison.right, decide_index, true, extremum_clause, false, outer, signs, source_alias,
			                 scope, sites);
		};
		walk(*decide.decide_constraints, string(), DecideEnclosingScope());
	}

	// The objective reads as one expression under its sense; a two-level `per` wrapper and a `when` carry no sign,
	// and the `when` keeps the rows its helper rows read. It is detached while its reducers are rewritten.
	unique_ptr<Expression> objective;
	const idx_t constraint_sites = sites.size();
	if (decide.decide_objective && HoldsReducer(*decide.decide_objective, decide_index, true)) {
		objective = std::move(decide.decide_objective);
		Expression *body = objective.get();
		DecideEnclosingScope scope;
		while (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conjunction = body->Cast<BoundConjunctionExpression>();
			if (!IsConstraintWrapper(conjunction) || conjunction.children.empty()) {
				break;
			}
			scope = EnterDecideScope(conjunction, scope);
			body = conjunction.children[0].get();
		}
		unordered_map<const Expression *, int> signs;
		CollectReducerSigns(context, *body, 1, decide_index, signs);
		const auto outer = decide.decide_sense == DecideSense::MINIMIZE ? ExtremumPush::DOWN : ExtremumPush::UP;
		CollectBodySites(*body, decide_index, false, false, false, outer, signs, OBJECTIVE_ROW_TAG, scope, sites);
	}

	if (!decide.use_native_constructs.min_max) {
		for (auto &site : sites) {
			if (site.needs_native_extremum) {
				RefuseWithoutNativeMinMax(site, decide);
			}
		}
	}

	// Group the copies before any body is replaced: a replaced body no longer compares equal.
	vector<vector<idx_t>> families;
	for (idx_t i = 0; i < sites.size(); i++) {
		auto family = std::find_if(families.begin(), families.end(), [&](const vector<idx_t> &members) {
			return SameReducer(sites[members[0]], sites[i]);
		});
		if (family == families.end()) {
			families.push_back({i});
		} else {
			family->push_back(i);
		}
	}

	using Kind = LogicalDecide::RewriteAccount::Kind;
	DecideCanonicalizer canonicalizer(context, decide_index, decide.variable_scopes, decide.keyed_scopes,
	                                  decide.is_boolean_var);
	// The rows are appended after both walks, so neither walks a row this pass added.
	vector<unique_ptr<Expression>> rows;
	for (idx_t family = 0; family < families.size(); family++) {
		auto &members = families[family];
		auto &first = sites[members[0]];
		bool down = false;
		bool up = false;
		bool none = false;
		for (auto member : members) {
			down |= sites[member].push == ExtremumPush::DOWN;
			up |= sites[member].push == ExtremumPush::UP;
			none |= sites[member].push == ExtremumPush::NONE;
		}
		const bool equal = none || (down && up);
		auto &body = first.reducer->children[0];
		const string body_text = RenderForPlan(*body, decide);
		// Whole-numbered when the body is, so a strict bound over the reducer still steps on the integer lattice.
		const bool whole = DecideValueIsWholeNumbered(*body, decide_index);
		// A column defined by the body: one per instance of the scopes it reads, its row one per instance.
		const auto scope = ScopeOf(decide, canonicalizer, *body);
		idx_t helper =
		    AddAuxiliaryDecision(decide, "__body_" + to_string(family) + "__",
		                         whole ? LogicalType::INTEGER : LogicalType::DOUBLE, false, scope, body_text);
		decide.signed_helper_vars.push_back(helper);
		// Each clause that reads the helper says so in its own plan, once.
		const auto kind = equal ? Kind::BODY_EQUAL : down ? Kind::BODY_AT_LEAST : Kind::BODY_AT_MOST;
		vector<string> told;
		for (auto member : members) {
			auto &alias = sites[member].source_alias;
			idx_t clause = DConstants::INVALID_INDEX;
			const string key = TryParseSourceClauseTag(alias, clause) ? to_string(clause) : alias;
			if (std::find(told.begin(), told.end(), key) == told.end()) {
				told.push_back(key);
				AddRewriteAccount(decide, alias, kind, body_text);
			}
		}

		const auto relation = equal  ? ExpressionType::COMPARE_EQUAL
		                      : down ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
		                             : ExpressionType::COMPARE_LESSTHANOREQUALTO;
		auto row = make_uniq<BoundComparisonExpression>(
		    relation, BoundCastExpression::AddCastToType(context, MakeDecisionRef(decide, helper), body->return_type),
		    body->Copy());
		MarkFormulationConstraint(*row, string());
		auto alias = row->GetAlias();
		AddDecideTag(alias, BODY_DEFINITION_TAG);
		row->SetAlias(std::move(alias));
		auto condition = HelperCondition(first);
		rows.push_back(ScopeRowPerInstance(decide, ScopeHelperRow(std::move(row), condition.get()), scope));

		for (auto member : members) {
			auto &reducer = *sites[member].reducer;
			ReplaceWithDecision(context, decide, helper, reducer.children[0]);
			AddDecideTag(reducer.alias, BODY_DEFINITION_TAG);
		}
	}

	if (objective) {
		if (sites.size() > constraint_sites) {
			decide.SetObjective(context, std::move(objective));
		} else {
			decide.decide_objective = std::move(objective);
		}
	}
	for (auto &row : rows) {
		AppendConstraint(decide, std::move(row));
	}
}

} // namespace duckdb
