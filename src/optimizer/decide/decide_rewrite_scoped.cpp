//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/optimizer/decide/decide_rewrite_scoped.cpp
//
// The value rewrite (syntax_reference.md §6.4; 05_optimizer/done.md, "The value
// rewrite"). A scoped comparison in the VALUE shape -- a decision reducer beside a
// per-row or keyed decision, under a factor that varies by row, next to a reducer with
// another `by` key, or inside another expression -- is not one model row per `by` group.
// Each of its decision reducers becomes a helper decision with one instance per `by`
// group, defined once (LogicalDecide::reducer_value_links; stage 06 emits the
// definition), and the comparison is then an ordinary row per class. Deck example 1,
// `ship <= 0.20 * sum(ship)`, costs N rows and one helper rather than N rows of N terms.
//
// A range frame over decisions (syntax_reference.md §5.6) is a reducer whose groups are
// the frame's windows, and becomes a helper through the same door (AddValueHelper,
// CollectDecisionSites; RewriteFrameRanges in decide_rewrite_frames.cpp).
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

void decide_rewrite::CollectDecisionSites(unique_ptr<Expression> &expr, int sign, DecisionSiteKind kind,
                                          ClientContext &context, const LogicalDecide &decide,
                                          vector<DecisionSite> &out) {
	if (!BoundExpressionReferencesDecide(*expr, decide.decide_index)) {
		return; // data holds no decision reducer and no range over decisions
	}
	switch (expr->GetExpressionClass()) {
	case ExpressionClass::BOUND_AGGREGATE: {
		idx_t frame;
		if (TryGetDecideFrame(*expr, frame)) {
			// A point is read in place, a tuple term of the row; a range over decisions is a
			// site of the range rewrite. No frame stands in a payload (D74).
			if (kind == DecisionSiteKind::RANGES && frame < decide.frames.size() && decide.frames[frame].range) {
				out.push_back({&expr, sign});
			}
			return;
		}
		if (kind == DecisionSiteKind::REDUCERS) {
			out.push_back({&expr, sign});
			return;
		}
		// A SUM or AVG passes its members' sign through, by positive weights; a MIN, a MAX
		// (D73) and a norm do not.
		auto &reducer = expr->Cast<BoundAggregateExpression>();
		auto name = StringUtil::Lower(reducer.function.name);
		string norm;
		const bool passes = (name == "sum" || name == "avg") && !TryParseNormMarker(reducer.GetAlias(), norm);
		for (auto &child : reducer.children) {
			CollectDecisionSites(child, passes ? sign : 0, kind, context, decide, out);
		}
		return;
	}
	case ExpressionClass::BOUND_CAST:
		CollectDecisionSites(expr->Cast<BoundCastExpression>().child, sign, kind, context, decide, out);
		return;
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr->Cast<BoundFunctionExpression>();
		auto &name = func.function.name;
		if (name == "+") {
			for (auto &child : func.children) {
				CollectDecisionSites(child, sign, kind, context, decide, out);
			}
			return;
		}
		if (name == "-" && func.children.size() == 2) {
			CollectDecisionSites(func.children[0], sign, kind, context, decide, out);
			CollectDecisionSites(func.children[1], -sign, kind, context, decide, out);
			return;
		}
		if (name == "-" && func.children.size() == 1) {
			CollectDecisionSites(func.children[0], -sign, kind, context, decide, out);
			return;
		}
		if ((name == "*" || name == "/") && func.children.size() == 2) {
			// `factor * term` and `term / factor`: the factor's sign, when it is known before
			// the query runs. A term in a divisor, or two decision-bearing factors, leave the
			// sign unknown.
			for (idx_t i = 0; i < 2; i++) {
				auto &term = func.children[i];
				auto &other = func.children[1 - i];
				if (!BoundExpressionReferencesDecide(*term, decide.decide_index)) {
					continue;
				}
				int factor_sign = 0;
				if (!(name == "/" && i == 1) && !BoundExpressionReferencesDecide(*other, decide.decide_index)) {
					factor_sign = ScaleSignAtPlanTime(context, other.get(), name == "/");
				}
				CollectDecisionSites(term, sign * factor_sign, kind, context, decide, out);
			}
			return;
		}
		break;
	}
	default:
		break;
	}
	// Any other node (ABS, POWER, CASE, ...): the sign of what it holds is not known.
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		CollectDecisionSites(child, 0, kind, context, decide, out);
	});
}

void decide_rewrite::ExtremumSides(const string &agg_name, ExtremumPush push, bool &need_envelope,
                                   bool &need_closing) {
	// Under `<=` a positive term is pushed down, so a MAX needs its envelope (`value >=
	// member`) and a MIN its closing side (`value >= some member`, with indicators); the
	// mirror for `>=` and for a negative term. An unknown sign, `=` and `<>` need both.
	const bool extremum = agg_name == "min" || agg_name == "max";
	const bool easy = IsEasyExtremum(agg_name == "max", push);
	need_envelope = extremum && (push == ExtremumPush::NONE || easy);
	need_closing = extremum && (push == ExtremumPush::NONE || !easy);
}

idx_t decide_rewrite::AddValueHelper(LogicalDecide &decide, LogicalDecide::ReducerValueLink link,
                                     unique_ptr<EntityScopeInfo> scope, bool whole,
                                     LogicalDecide::RewriteAccount::Kind account) {
	const idx_t link_idx = decide.reducer_value_links.size();
	// One instance per entity of its scope, which execution builds from the link, or one
	// for the query.
	auto var_scope = DecideVarScopeInfo::Scalar();
	if (scope) {
		scope->value_link = link_idx;
		decide.entity_scopes.push_back(std::move(*scope));
		var_scope = DecideVarScopeInfo::Entity(decide.entity_scopes.size() - 1);
	}
	// F6: a diagnosis, and a plan, name the helper by what it holds, `by` included.
	link.value_var = AddAuxiliaryDecision(decide, "__value_" + to_string(link_idx) + "__",
	                                      whole ? LogicalType::INTEGER : LogicalType::DOUBLE, false, var_scope,
	                                      link.label);
	if (var_scope.IsEntity()) {
		decide.entity_scopes[var_scope.entity_scope_idx].scoped_variable_indices.push_back(link.value_var);
	}
	AddRewriteAccount(decide, link.source_clause_id, account, link.label);
	decide.reducer_value_links.push_back(std::move(link));
	return link_idx;
}

namespace {

using ReducerSite = DecisionSite;

//! The helper decisions allocated so far, each with the reducer it holds as written (the
//! alias carries its `by` key and qualifier). One occurrence of a reducer in a clause is
//! one helper, however many rows the rewrites before this one derived from that clause:
//! an ABS splits a clause into an envelope pair, each row holding a copy of the reducer,
//! and two helpers pinned on different sides would let the pair drift apart. The rows a
//! rewrite added for the objective (OBJECTIVE_ROW_TAG) share theirs the same way. Rows of
//! different clauses never share one: the definition carries one clause's provenance.
struct Helpers {
	LogicalDecide &decide;
	vector<unique_ptr<Expression>> reducers; //!< each helper's reducer, as written
	vector<idx_t> links;                     //!< parallel: its link, into decide.reducer_value_links
	vector<bool> objective;                  //!< parallel: the helper serves the objective

	//! The link holding `agg` for a row of the clause `source_clause_id` (or of the
	//! objective), reading `rows` grouped by `group_key` -- its path's (syntax_reference.md §5.5) --, or
	//! INVALID_INDEX.
	idx_t Find(const BoundAggregateExpression &agg, const Expression *rows, idx_t group_key, idx_t source_clause_id,
	           bool objective_row) const {
		if (source_clause_id == DConstants::INVALID_INDEX && !objective_row) {
			return DConstants::INVALID_INDEX;
		}
		// Other links -- a range frame's (RewriteFrameRanges) -- stand among these.
		for (idx_t i = 0; i < reducers.size(); i++) {
			auto &link = decide.reducer_value_links[links[i]];
			if (link.source_clause_id != source_clause_id || objective[i] != objective_row ||
			    link.group_key != group_key || !reducers[i]->Equals(agg) ||
			    reducers[i]->GetAlias() != agg.GetAlias()) {
				continue;
			}
			// A clause's rows carry copies of its `when`: the same condition.
			const bool same_rows = rows ? link.when && link.when->Equals(*rows) : !link.when;
			if (same_rows) {
				return links[i];
			}
		}
		return DConstants::INVALID_INDEX;
	}
};

//! Replace the reducer at `site` by its helper decision: the clause's existing one for a
//! repeated occurrence, else a new one, recording what it holds. The reducers inside its
//! argument are replaced first, each by a helper keyed by its path (syntax_reference.md §5.5), so that what the
//! helper holds is affine in helpers, decisions and data. `push` is how the row pushes
//! the reducer's value; `comparison` is the row's, for a reducer at the top of its side,
//! and COMPARE_EQUAL for a nested one, whose own value no comparison steps.
void ReplaceReducer(Helpers &helpers, const DecideSourceNames &names, const DecideReducerPath &path,
                    ExtremumPush push, ExpressionType comparison, idx_t source_clause_id, idx_t removal_group_id,
                    bool objective_row, ClientContext &context, const ReducerSite &site) {
	auto &decide = helpers.decide;
	auto &agg = (*site.slot)->Cast<BoundAggregateExpression>();
	auto agg_name = StringUtil::Lower(agg.function.name);
	const bool extremum = agg_name == "min" || agg_name == "max";
	// Named as written, before the reducers inside it become helpers.
	auto label = RenderDecideReducer(agg, names);
	auto plan_label = RenderDecideSource(agg, names);
	// A member of a SUM or AVG is pushed as the helper is, through the sign it carries in
	// the argument; a member of a MIN or MAX is pinned from both sides (D73).
	vector<ReducerSite> nested;
	if (!agg.children.empty()) {
		CollectDecisionSites(agg.children[0], 1, DecisionSiteKind::REDUCERS, context, decide, nested);
	}
	auto below = path.Below(agg);
	for (auto &inner : nested) {
		const auto inner_push = extremum ? ExtremumPush::NONE : ExtremumPushOf(push, inner.sign);
		ReplaceReducer(helpers, names, below, inner_push, ExpressionType::COMPARE_EQUAL, source_clause_id,
		               removal_group_id, objective_row, context, inner);
	}
	// syntax_reference.md §10.2: a product or square of decisions inside a reducer is
	// accepted only where the reducer's own row sums it -- the aggregate shape. Here its
	// definition would be a quadratic equality. The test is on what survives the rewrites
	// before this one: an ABS is an auxiliary by now and a Boolean product its McCormick
	// column, both linear; what remains nonlinear is a square or a product of two
	// non-Boolean decisions.
	if (!DecideExpressionDegree(*agg.children[0], decide.decide_index).IsLinear()) {
		// Among nested reducers every one is a helper, so none is ever summed in place (D74).
		if (!path.keys.empty() || !nested.empty()) {
			throw BinderException("DECIDE: '%s' squares or multiplies decisions inside nested reducers, which is not "
			                      "supported yet",
			                      label);
		}
		throw BinderException("DECIDE: '%s' is not supported yet in this constraint: a product or square of "
		                      "decisions inside a reducer is accepted only where the constraint's reducers share "
		                      "one by key, under a whole-query factor, with nothing beside them but data and "
		                      "query-wide decisions, each on its own",
		                      label);
	}

	// MIN/MAX: which side pins the helper is the one the row would otherwise let it escape
	// through (ExtremumSides). A helper several rows read needs every side any of them does.
	bool need_envelope, need_closing;
	ExtremumSides(agg_name, push, need_envelope, need_closing);

	// `<`, `>` and `<>` step onto the whole numbers, which is exact for the row only when
	// the helper is whole. The binder admitted the clause by its own reading: a constant
	// beside the decisions inside a SUM moves to the bound when the reducer is summed in
	// place (`sum(x + 0.5) > 4` is `sum(x) > 4 - 0.5 * n`), but a helper holds the whole
	// value. The message is the binder's (ValidateDecideIntegralComparisonOperands).
	const bool whole = DecideValueIsWholeNumbered(agg, decide.decide_index);
	if (!whole && (comparison == ExpressionType::COMPARE_LESSTHAN ||
	               comparison == ExpressionType::COMPARE_GREATERTHAN || comparison == ExpressionType::COMPARE_NOTEQUAL)) {
		const bool not_equal = comparison == ExpressionType::COMPARE_NOTEQUAL;
		const bool less = comparison == ExpressionType::COMPARE_LESSTHAN;
		throw BinderException("Comparison '%s' is not supported here: '%s' can take fractional values in this "
		                      "constraint, and %s. Compare with %s instead.",
		                      not_equal ? "<>" : less ? "<" : ">", RenderDecideSource(agg, names),
		                      not_equal ? "the rewrite (x <> K becomes x <= K-1 OR x >= K+1) is exact only on "
		                                  "whole numbers"
		                                : "stepping the bound is exact only on whole numbers",
		                      not_equal ? "'<=' or '>='" : less ? "'<='" : "'>='");
	}

	// It reads the rows its path keeps, grouped by its path key: for a reducer at the top,
	// the clause's kept rows by its own `by` key.
	auto rows = path.Rows();
	const idx_t group_key = FindOrCreateDecidePathKey(path.KeysOf(DecideReducerGroupKey(agg)), decide.group_keys);
	idx_t link_idx = helpers.Find(agg, rows.get(), group_key, source_clause_id, objective_row);
	if (link_idx != DConstants::INVALID_INDEX) {
		auto &link = decide.reducer_value_links[link_idx];
		link.need_envelope = link.need_envelope || need_envelope;
		link.need_closing = link.need_closing || need_closing;
		*site.slot = MakeDecisionRef(decide, link.value_var);
		return;
	}

	LogicalDecide::ReducerValueLink link;
	link.agg_name = agg_name;
	// Its columns are named by the reducer it holds, `by` included: a refusal's remedy
	// bounds that (F6).
	link.label = plan_label;
	link.group_key = group_key;
	TryParseQualifiedReducerTag(agg.alias, link.qualifier_scope_idx);
	link.inner_expr = agg.children[0]->Copy();
	if (agg.filter) {
		link.filter = agg.filter->Copy();
	}
	link.when = std::move(rows);
	link.source_clause_id = source_clause_id;
	link.removal_group_id = removal_group_id;
	link.need_envelope = need_envelope;
	link.need_closing = need_closing;

	// The helper decision: whole-numbered when the reducer's value is, so a strict bound on
	// the rewritten row still steps on the integer lattice; a query-wide decision when its
	// path key is `by ()` throughout, else one instance per group of that key among the rows
	// its path keeps -- a scope of its own (EntityScopeInfo::value_link), which execution
	// builds from the link.
	unique_ptr<EntityScopeInfo> scope;
	if (link.group_key != DConstants::INVALID_INDEX) {
		scope = make_uniq<EntityScopeInfo>();
		scope->table_alias = decide.group_keys[link.group_key].text;
		scope->key_names = decide.group_keys[link.group_key].names;
	}
	const auto account = scope ? LogicalDecide::RewriteAccount::Kind::VALUE_HELPER_PER_GROUP
	                           : LogicalDecide::RewriteAccount::Kind::VALUE_HELPER;
	link_idx = AddValueHelper(decide, std::move(link), std::move(scope), whole, account);
	helpers.reducers.push_back(agg.Copy());
	helpers.links.push_back(link_idx);
	helpers.objective.push_back(objective_row);
	*site.slot = MakeDecisionRef(decide, decide.reducer_value_links[link_idx].value_var);
}

//! The key a reducer's own `per` names, as expressions: its key scope's columns, or for a
//! scope the canonicalizer split by a signature (D65), that signature's key.
vector<unique_ptr<Expression>> QualifierKey(const LogicalDecide &decide, idx_t scope_idx) {
	vector<unique_ptr<Expression>> key;
	auto &scope = decide.entity_scopes[scope_idx];
	if (scope.signature != DConstants::INVALID_INDEX) {
		for (auto &column : decide.signatures[scope.signature].key) {
			key.push_back(column->Copy());
		}
		return key;
	}
	for (idx_t i = 0; i < scope.entity_key_bindings.size(); i++) {
		key.push_back(make_uniq<BoundColumnRefExpression>(scope.entity_key_column_types[i],
		                                                  scope.entity_key_bindings[i]));
	}
	return key;
}

//! `sum([when c] per K: inner([when c'] e) by (G))` in the objective, `inner` a SUM or AVG
//! with no `per` and no reducer inside, every column of K in G: each G group is one
//! member, so the sum of the members is `inner` read over every group, `c` joined to `c'`.
//! Written so, the objective sums -- or averages per G group -- `e` itself, a square
//! included, which a helper could not define linearly (D75). Replaces the reducer at
//! `slot` and returns true; false when it is not that shape.
bool InlineSumOfGroupSums(LogicalDecide &decide, ClientContext &context, unique_ptr<Expression> &slot) {
	auto &outer = slot->Cast<BoundAggregateExpression>();
	idx_t qualifier;
	if (!StringUtil::CIEquals(outer.function.name, "sum") || outer.children.size() != 1 ||
	    DecideReducerGroupKey(outer) != DConstants::INVALID_INDEX ||
	    !TryParseQualifiedReducerTag(outer.GetAlias(), qualifier) || qualifier >= decide.entity_scopes.size()) {
		return false;
	}
	auto *body = UnwrapDecideCasts(*outer.children[0], decide.decide_index);
	if (body->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	auto &inner = body->Cast<BoundAggregateExpression>();
	idx_t inner_qualifier;
	const bool sum_or_avg =
	    StringUtil::CIEquals(inner.function.name, "sum") || StringUtil::CIEquals(inner.function.name, "avg");
	if (!sum_or_avg || inner.children.size() != 1 || DecideCanonicalizer::ContainsReducer(*inner.children[0]) ||
	    TryParseQualifiedReducerTag(inner.GetAlias(), inner_qualifier)) {
		return false;
	}
	vector<unique_ptr<Expression>> no_key;
	auto group_key = DecideReducerGroupKey(inner);
	auto &by = group_key == DConstants::INVALID_INDEX ? no_key : decide.group_keys[group_key].expressions;
	for (auto &column : QualifierKey(decide, qualifier)) {
		bool in_by = false;
		for (auto &part : by) {
			in_by = in_by || part->Equals(*column);
		}
		if (!in_by) {
			return false; // a G group holding two classes of K would be counted twice
		}
	}
	auto inlined = inner.Copy();
	auto &reducer = inlined->Cast<BoundAggregateExpression>();
	if (outer.filter && reducer.filter) {
		auto both = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		both->children.push_back(outer.filter->Copy());
		both->children.push_back(std::move(reducer.filter));
		reducer.filter = std::move(both);
	} else if (outer.filter) {
		reducer.filter = outer.filter->Copy();
	}
	slot = BoundCastExpression::AddCastToType(context, std::move(inlined), outer.return_type);
	return true;
}

} // namespace

void DecideOptimizer::RewriteScopedReducers(LogicalDecide &decide) {
	if (!decide.decide_constraints && !decide.decide_objective) {
		return;
	}
	auto names = DecidePlanNames(decide.source_fragments, decide.entity_scopes, decide.group_keys, decide.decide_index,
	                             decide.aux_var_expressions);
	Helpers helpers {decide, {}, {}, {}};

	std::function<void(unique_ptr<Expression> &, ClauseScope)> rewrite = [&](unique_ptr<Expression> &expr,
	                                                                         ClauseScope scope) {
		scope = EnterClauseScope(*expr, scope);
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conj = expr->Cast<BoundConjunctionExpression>();
			for (idx_t i = 0; i < conj.children.size(); i++) {
				if (IsConstraintChild(conj, i)) {
					rewrite(conj.children[i], scope);
				}
			}
			return;
		}
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON ||
		    !HasDecideTag(expr->GetAlias(), VALUE_REWRITE_TAG)) {
			return;
		}
		auto &cmp = expr->Cast<BoundComparisonExpression>();
		idx_t source_clause_id = DConstants::INVALID_INDEX;
		idx_t removal_group_id = DConstants::INVALID_INDEX;
		TryParseSourceClauseTag(cmp.GetAlias(), source_clause_id);
		TryParseRemovalGroupTag(cmp.GetAlias(), removal_group_id);
		// Canonicalization put every decision reducer on the left.
		vector<ReducerSite> sites;
		CollectDecisionSites(cmp.left, 1, DecisionSiteKind::REDUCERS, optimizer.context, decide, sites);
		const bool objective_row = HasDecideTag(cmp.GetAlias(), OBJECTIVE_ROW_TAG);
		DecideReducerPath path;
		if (scope.when) {
			path.conditions.push_back(scope.when);
		}
		for (auto &site : sites) {
			ReplaceReducer(helpers, names, path, ExtremumPushOf(ExtremumPushOf(cmp.type), site.sign), cmp.type,
			               source_clause_id, removal_group_id, objective_row, optimizer.context, site);
		}
		// One row per class from here on: the shape tag has done its work.
		auto alias = cmp.GetAlias();
		RemoveDecideTag(alias, VALUE_REWRITE_TAG);
		cmp.SetAlias(std::move(alias));
	};
	if (decide.decide_constraints) {
		rewrite(decide.decide_constraints, ClauseScope());
	}
	if (!decide.decide_objective) {
		return;
	}
	// The objective's nested reducers (D75): each reducer the canonicalizer tagged becomes a
	// helper decision, and every reducer inside it one keyed by its path, so the objective
	// reads the helper once. Its value is pushed the way the sense pushes the objective.
	auto scope = EnterClauseScope(*decide.decide_objective, ClauseScope());
	auto *body = &decide.decide_objective;
	if (scope.when) {
		body = &decide.decide_objective->Cast<BoundConjunctionExpression>().children[0];
	}
	vector<ReducerSite> sites;
	CollectDecisionSites(*body, 1, DecisionSiteKind::REDUCERS, optimizer.context, decide, sites);
	DecideReducerPath path;
	if (scope.when) {
		path.conditions.push_back(scope.when);
	}
	const auto sense = decide.decide_sense == DecideSense::MAXIMIZE ? ExtremumPush::UP : ExtremumPush::DOWN;
	bool rewritten = false;
	for (auto &site : sites) {
		if (!HasDecideTag((*site.slot)->GetAlias(), VALUE_REWRITE_TAG)) {
			continue;
		}
		rewritten = true;
		if (InlineSumOfGroupSums(decide, optimizer.context, *site.slot)) {
			continue;
		}
		ReplaceReducer(helpers, names, path, ExtremumPushOf(sense, site.sign), ExpressionType::COMPARE_EQUAL,
		               DConstants::INVALID_INDEX, DConstants::INVALID_INDEX, /*objective_row=*/true,
		               optimizer.context, site);
	}
	if (rewritten) {
		auto objective = std::move(decide.decide_objective);
		decide.SetObjective(optimizer.context, std::move(objective));
	}
}

} // namespace duckdb
