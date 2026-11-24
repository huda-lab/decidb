//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/optimizer/decide/decide_tree_definition.cpp
//
// Prepare the logical metadata for definition F nodes before later formulation passes read their values.
//
//===----------------------------------------------------------------------===//

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"
#include "duckdb/optimizer/decide/decide_solver_gate.hpp"
#include "duckdb/planner/decide/decide_shape_check.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/decide/decide_tree.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include <algorithm>

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

// ---------------------------------------------------------------------------
// Defining a non-linear reducer body (the Plaisted-Greenbaum encoding)
// ---------------------------------------------------------------------------
//
// Every extremum the later passes build reads its members as linear: the
// value rewrite's helpers, a MIN or MAX's included, whatever clause or objective reads it. A body that squares or
// multiplies decisions (`max(power(x, 2))`, once RewriteBilinear has taken the Boolean products) would lose its square
// there. Tree preparation names such a body first: one helper decision `t` per instance of the scopes the body reads
// (ScopeOf: per row once it reads a data column) and one quadratic row defining it by the body `q` per instance, so the
// reducer reads the bare column `t` and every later pass keeps reading linear members. RewriteAbs does the same for an
// ABS.
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
// AVG term's too (the clause row reads them as linear); in a constraint the value rewrite
// takes (VALUE shape), every SUM and AVG term's (the helper decision that holds the reducer
// is defined over linear members, and it is the same epigraph form); and in the objective,
// every reducer's under a MIN or MAX, a nested one's included. A plain constraint whose one
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

//! A SUM, AVG, MIN or MAX over decisions.
optional_ptr<const BoundDecideReducerExpression> AsDecisionReducer(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_DECIDE_REDUCER) {
		return nullptr;
	}
	auto &agg = expr.Cast<BoundDecideReducerExpression>();
	if (agg.kind == DecideReducerKind::COUNT || agg.kind == DecideReducerKind::NORM || agg.children.size() != 1 ||
	    !BoundExpressionReferencesDecide(*agg.children[0], decide_index)) {
		return nullptr;
	}
	return &agg;
}

//! Does `expr` hold a reducer over decisions (with `extremum_only`, a MIN or MAX) at or below it?
bool HoldsReducer(const Expression &expr, idx_t decide_index, bool extremum_only) {
	auto reducer = AsDecisionReducer(expr, decide_index);
	if (reducer && (!extremum_only || IsDecideExtremum(reducer->kind))) {
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
	BoundDecideReducerExpression *reducer;
	ExtremumPush push;
	//! A SUM or AVG term beside a MIN or MAX, rather than one or a reducer inside one: what the refusal says.
	bool beside_extremum;
	//! Whether a MIN or MAX reads this body, so the backend must state MIN/MAX natively: the reducer is one, sits
	//! inside one, or sits in a constraint that holds one.
	bool needs_native_extremum;
	DecideRowOrigin origin;
	//! The clause's `when`, or null.
	const Expression *when;
};

//! The sign every reducer in `side` carries (WalkPolarity), keyed by node.
void CollectReducerSigns(ClientContext &context, const BooleanDecisions &booleans, unique_ptr<Expression> &side,
                         int sign, unordered_map<const Expression *, int> &signs) {
	const idx_t decide_index = booleans.decide_index;
	PolarityHolds holds = [&](const Expression &node) {
		return HoldsReducer(node, decide_index, false);
	};
	PolarityVisit visit = [&](unique_ptr<Expression> &node, int node_sign, const PolarityPath &) {
		if (AsDecisionReducer(*node, decide_index)) {
			signs[node.get()] = node_sign;
		}
		return false;
	};
	WalkPolarity(context, booleans, side, sign, holds, visit);
}

//! The reducers under `expr` whose body is not linear and holds no reducer. A SUM or AVG qualifies with `sums`
//! (a constraint that holds a MIN or MAX) or `under_extremum` (a reducer inside a MIN or MAX). A reducer
//! the polarity walk did not reach (`signs`) is pushed neither way.
void CollectBodySites(Expression &expr, idx_t decide_index, const DecideTermSplitter &splitter, bool sums,
                      bool extremum_clause, bool under_extremum, ExtremumPush outer,
                      const unordered_map<const Expression *, int> &signs, const DecideRowOrigin &origin,
                      const Expression *when, vector<BodySite> &sites) {
	if (!AsDecisionReducer(expr, decide_index)) {
		ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
			CollectBodySites(child, decide_index, splitter, sums, extremum_clause, under_extremum, outer, signs, origin,
			                 when, sites);
		});
		return;
	}
	auto &reducer = expr.Cast<BoundDecideReducerExpression>();
	const bool is_extremum = IsDecideExtremum(reducer.kind);
	auto &body = *reducer.children[0];
	if (HoldsReducer(body, decide_index, false)) {
		CollectBodySites(body, decide_index, splitter, sums, extremum_clause, is_extremum, outer, signs, origin, when,
		                 sites);
		return;
	}
	if (splitter.IsLinear(body) || (!is_extremum && !sums && !under_extremum)) {
		return;
	}
	auto sign = signs.find(&expr);
	const auto push = sign == signs.end() ? ExtremumPush::NONE : ExtremumPushOf(outer, sign->second);
	sites.push_back({&reducer, push, !is_extremum && !under_extremum, is_extremum || under_extremum || extremum_clause,
	                 origin, when});
}

//! Copies of one reducer under one `when`: they share a helper.
bool SameReducer(const BodySite &a, const BodySite &b) {
	if (!Expression::Equals(*a.reducer, *b.reducer)) {
		return false;
	}
	if (!a.when || !b.when) {
		return !a.when && !b.when;
	}
	return Expression::Equals(*a.when, *b.when);
}

//! The clause's `when` and the reducer's own filter: the rows a helper is defined on.
unique_ptr<Expression> HelperCondition(const BodySite &site) {
	auto when = site.when;
	if (!when || !site.reducer->filter) {
		return when ? when->Copy() : site.reducer->filter ? site.reducer->filter->Copy() : nullptr;
	}
	return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, when->Copy(),
	                                             site.reducer->filter->Copy());
}

[[noreturn]] void RefuseWithoutNativeMinMax(const BodySite &site) {
	// The reducer is the place: the error points at it.
	ThrowDecideSolverGap("MIN or MAX over a squared or multiplied expression", site.reducer->GetQueryLocation());
}

} // namespace

void DecideTreeConstruction::PrepareDefinitionFunctions(ClientContext &context, LogicalDecide &decide) {
	const idx_t decide_index = decide.decide_index;
	DecideTermSplitter splitter(context, decide);
	const BooleanDecisions booleans {decide_index, decide.bound.is_boolean_var};
	vector<BodySite> sites;

	// The objective reads as one minimized expression; a `when` carries no sign, and the `when` keeps the rows its
	// helper rows read. Its reducers are rewritten in place, so it goes back through the boundary when any is.
	bool objective_defined = false;
	decide.EnumerateBodies([&](DecideOwnedBody &body) {
		if (body.owner == DecideOwnedBody::Owner::CLAUSE) {
			auto &expr = *body.expr;
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
				return;
			}
			// A MIN or MAX in the constraint, or any reducer the value rewrite holds in a helper decision.
			const bool extremum_clause = HoldsReducer(expr, decide_index, true);
			if (!extremum_clause &&
			    !(body.clause->shape == CanonicalConstraintClass::VALUE && HoldsReducer(expr, decide_index, false))) {
				return;
			}
			auto &comparison = expr.Cast<BoundComparisonExpression>();
			// A MIN or MAX pushed its cheap way (`max(q) <= K`, RewriteMinMax) bounds each member: one quadratic row
			// per member, and no helper.
			if (body.clause->minmax_easy) {
				return;
			}
			// Read as `LHS - RHS <op> 0`: the relation pushes the whole expression.
			unordered_map<const Expression *, int> signs;
			CollectReducerSigns(context, booleans, comparison.left, 1, signs);
			CollectReducerSigns(context, booleans, comparison.right, -1, signs);
			const auto outer = ExtremumPushOf(comparison.type);
			CollectBodySites(*comparison.left, decide_index, splitter, true, extremum_clause, false, outer, signs,
			                 body.Origin(), body.rows, sites);
			CollectBodySites(*comparison.right, decide_index, splitter, true, extremum_clause, false, outer, signs,
			                 body.Origin(), body.rows, sites);
		} else if (body.owner == DecideOwnedBody::Owner::OBJECTIVE && HoldsReducer(*body.expr, decide_index, true)) {
			unordered_map<const Expression *, int> signs;
			CollectReducerSigns(context, booleans, body.expr, 1, signs);
			const idx_t before = sites.size();
			CollectBodySites(*body.expr, decide_index, splitter, false, false, false, ExtremumPush::DOWN, signs,
			                 body.Origin(), body.rows, sites);
			objective_defined = sites.size() > before;
		}
	});

	if (!decide.use_native_constructs.min_max) {
		for (auto &site : sites) {
			if (site.needs_native_extremum) {
				RefuseWithoutNativeMinMax(site);
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
	DecideCanonicalizer canonicalizer(context, decide_index, decide.bound);
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
		    AddAuxiliaryDecision(decide, string(BODY_NAME_PREFIX) + to_string(family) + "__",
		                         whole ? LogicalType::INTEGER : LogicalType::DOUBLE, false, scope, body_text);
		// Each clause that reads the helper says so in its own plan, once.
		const auto kind = equal ? Kind::BODY_EQUAL : down ? Kind::BODY_AT_LEAST : Kind::BODY_AT_MOST;
		vector<std::pair<idx_t, bool>> told;
		for (auto member : members) {
			auto &origin = sites[member].origin;
			const auto key = std::make_pair(origin.source_clause_id, origin.objective);
			if (std::find(told.begin(), told.end(), key) == told.end()) {
				told.push_back(key);
				AddRewriteAccount(decide, origin.source_clause_id, kind, body_text);
			}
		}

		const auto relation = equal  ? ExpressionType::COMPARE_EQUAL
		                      : down ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
		                             : ExpressionType::COMPARE_LESSTHANOREQUALTO;
		auto condition = HelperCondition(first);
		auto definition = make_uniq<DecideBodyDefinition>();
		definition->helper = helper;
		definition->relation = relation;
		definition->body = body->Copy();
		definition->when = condition ? condition->Copy() : nullptr;
		// A message about the row quotes the first clause that reads the helper. Only that id is kept: the row is
		// shared by every clause that reads the helper, so it joins none of their removal groups.
		definition->origin = DecideRowOrigin::OfClause(first.origin.source_clause_id);
		decide.helper_definitions.push_back(std::move(definition));
		const idx_t definition_index = decide.helper_definitions.size() - 1;

		for (auto member : members) {
			auto &reducer = *sites[member].reducer;
			// Keep the nonlinear expression where it was written. The tree lowers the call as a read of the definition
			// F node, while the term splitter reads the same helper column in optimizer-owned formulations such as
			// hard MIN/MAX. The F node's retained body above names no definition, so its own equation compiles the
			// source expression instead of recursively reading itself.
			// The call under the casts names it: every reader looks through a cast (UnwrapDecideCasts) first. A
			// product that already names its expansion (FindAndReplaceBilinear) names the definition from here on,
			// which every reader asks for first; the retained body keeps the expansion's.
			decide.NameDefinition(*UnwrapDecideCasts(*reducer.children[0], decide_index), definition_index);
		}
	}

	if (objective_defined) {
		decide.SetObjective(context, std::move(decide.bound.objective));
	}
}

void DecideBodyDefinition::Rows(ClientContext &context, const LogicalDecide &decide,
                                const std::function<void(const BoundDecideClause &)> &callback) const {
	auto left = BoundCastExpression::AddCastToType(context, decide.bound.variables[helper]->Copy(), body->return_type);
	auto row =
	    MakeFormulationRow(make_uniq<BoundComparisonExpression>(relation, std::move(left), body->Copy()), origin);
	if (helper < decide.bound.variable_scopes.size()) {
		auto &scope = decide.bound.variable_scopes[helper];
		if (scope.IsGlobal() || scope.IsKeyed()) {
			row.scope = DecideClauseScope::Per(scope.IsKeyed() ? decide.bound.keyed_scopes[scope.keyed_scope_idx].key
			                                                   : DConstants::INVALID_INDEX);
		}
	}
	row.scope.when = when ? when->Copy() : nullptr;
	callback(row);
}

} // namespace duckdb
