//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/optimizer/decide/decide_rewrite_scoped.cpp
//
// The value rewrite (language/guide.md §6.4; 05_optimizer/done.md, "The value
// rewrite"). A scoped comparison in the VALUE shape -- a decision reducer beside a
// per-row or keyed decision, under a factor that varies by row, next to a reducer with
// another `by` key, or inside another expression -- is not one model row per `by` group.
// Each of its decision reducers becomes a helper decision with one instance per `by`
// group, defined once (LogicalDecide::reducer_value_links; stage 06 emits the
// definition), and the comparison is then an ordinary row per class. Deck example 1,
// `ship <= 0.20 * sum(ship)`, costs N rows and one helper rather than N rows of N terms.
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

namespace {

//! A decision reducer on the decision side of a VALUE-shape comparison, and the sign its
//! term carries once the side is read as a sum: through `+`, `-`, binder casts and the
//! factors on it. 0 when that sign is not known before the query runs -- a factor that is
//! a data column or a subquery, a reducer inside another expression -- which pins a
//! MIN/MAX helper from both sides.
struct ReducerSite {
	unique_ptr<Expression> *slot;
	int sign;
};

void CollectReducerSites(unique_ptr<Expression> &expr, int sign, ClientContext &context, idx_t decide_index,
                         vector<ReducerSite> &out) {
	if (!BoundExpressionReferencesDecide(*expr, decide_index)) {
		return; // data holds no decision reducer
	}
	switch (expr->GetExpressionClass()) {
	case ExpressionClass::BOUND_AGGREGATE:
		out.push_back({&expr, sign});
		return;
	case ExpressionClass::BOUND_CAST:
		CollectReducerSites(expr->Cast<BoundCastExpression>().child, sign, context, decide_index, out);
		return;
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr->Cast<BoundFunctionExpression>();
		auto &name = func.function.name;
		if (name == "+") {
			for (auto &child : func.children) {
				CollectReducerSites(child, sign, context, decide_index, out);
			}
			return;
		}
		if (name == "-" && func.children.size() == 2) {
			CollectReducerSites(func.children[0], sign, context, decide_index, out);
			CollectReducerSites(func.children[1], -sign, context, decide_index, out);
			return;
		}
		if (name == "-" && func.children.size() == 1) {
			CollectReducerSites(func.children[0], -sign, context, decide_index, out);
			return;
		}
		if ((name == "*" || name == "/") && func.children.size() == 2) {
			// `factor * reducer` and `reducer / factor`: the factor's sign, when it is known
			// before the query runs. A reducer in a divisor, or two decision-bearing
			// factors, leave the sign unknown.
			for (idx_t i = 0; i < 2; i++) {
				auto &term = func.children[i];
				auto &other = func.children[1 - i];
				if (!BoundExpressionReferencesDecide(*term, decide_index)) {
					continue;
				}
				int factor_sign = 0;
				if (!(name == "/" && i == 1) && !BoundExpressionReferencesDecide(*other, decide_index)) {
					factor_sign = ScaleSignAtPlanTime(context, other.get(), name == "/");
				}
				CollectReducerSites(term, sign * factor_sign, context, decide_index, out);
			}
			return;
		}
		break;
	}
	default:
		break;
	}
	// Any other node (ABS, POWER, CASE, ...): the sign of what it holds is not known.
	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { CollectReducerSites(child, 0, context, decide_index, out); });
}

//! The helper decisions allocated so far, each with the reducer it holds as written (the
//! alias carries its `by` key and qualifier). One occurrence of a reducer in a clause is
//! one helper, however many rows the rewrites before this one derived from that clause:
//! an ABS splits a clause into an envelope pair, each row holding a copy of the reducer,
//! and two helpers pinned on different sides would let the pair drift apart. The rows a
//! rewrite added for the objective (OBJECTIVE_ROW_TAG) share theirs the same way. Rows of
//! different clauses never share one: the definition carries one clause's provenance.
struct Helpers {
	LogicalDecide &decide;
	vector<unique_ptr<Expression>> reducers; //!< parallel to decide.reducer_value_links
	vector<bool> objective;                  //!< parallel: the helper serves the objective

	//! The link holding `agg` for a row of the clause `source_clause_id` (or of the
	//! objective) in `scope`, or INVALID_INDEX.
	idx_t Find(const BoundAggregateExpression &agg, const ClauseScope &scope, idx_t source_clause_id,
	           bool objective_row) const {
		if (source_clause_id == DConstants::INVALID_INDEX && !objective_row) {
			return DConstants::INVALID_INDEX;
		}
		for (idx_t i = 0; i < reducers.size(); i++) {
			auto &link = decide.reducer_value_links[i];
			if (link.source_clause_id != source_clause_id || objective[i] != objective_row ||
			    !reducers[i]->Equals(agg) || reducers[i]->GetAlias() != agg.GetAlias()) {
				continue;
			}
			// A clause's rows carry copies of its `when`: the same condition.
			const bool same_when = scope.when ? link.when && link.when->Equals(*scope.when) : !link.when;
			if (same_when) {
				return i;
			}
		}
		return DConstants::INVALID_INDEX;
	}
};

//! Replace the reducer at `site` by its helper decision: the clause's existing one for a
//! repeated occurrence, else a new one, recording what it holds.
void ReplaceReducer(Helpers &helpers, const DecideSourceNames &names, const ClauseScope &scope,
                    ExpressionType comparison, idx_t source_clause_id, idx_t removal_group_id, bool objective_row,
                    const ReducerSite &site) {
	auto &decide = helpers.decide;
	auto &agg = (*site.slot)->Cast<BoundAggregateExpression>();
	auto agg_name = StringUtil::Lower(agg.function.name);
	auto label = RenderDecideReducer(agg, names);
	// language/errors.md §2: a product or square of decisions inside a reducer is
	// accepted only where the reducer's own row sums it -- the aggregate shape. Here its
	// definition would be a quadratic equality. The test is on what survives the rewrites
	// before this one: an ABS is an auxiliary by now and a Boolean product its McCormick
	// column, both linear; what remains nonlinear is a square or a product of two
	// non-Boolean decisions.
	if (!DecideExpressionDegree(*agg.children[0], decide.decide_index).IsLinear()) {
		throw BinderException("DECIDE: '%s' is not supported yet in this constraint: a product or square of "
		                      "decisions inside a reducer is accepted only where the constraint's reducers share "
		                      "one by key, under a whole-query factor, with nothing beside them but data and "
		                      "query-wide decisions, each on its own",
		                      label);
	}

	// MIN/MAX: which side pins the helper is the one the row would otherwise let it escape
	// through. Under `<=` a positive term is pushed down, so a MAX needs its envelope
	// (`value >= member`) and a MIN its closing side (`value >= some member`, with
	// indicators); the mirror for `>=` and for a negative term. An unknown sign, `=` and
	// `<>` need both. A helper several rows read needs every side any of them does.
	const bool extremum = agg_name == "min" || agg_name == "max";
	const ExtremumPush push = ExtremumPushOf(ExtremumPushOf(comparison), site.sign);
	const bool easy = IsEasyExtremum(agg_name == "max", push);
	const bool need_envelope = extremum && (push == ExtremumPush::NONE || easy);
	const bool need_closing = extremum && (push == ExtremumPush::NONE || !easy);

	// `<`, `>` and `<>` step onto the whole numbers, which is exact for the row only when
	// the helper is whole. The binder admitted the clause by its own reading: a constant
	// beside the decisions inside a SUM moves to the bound when the reducer is summed in
	// place (`sum(x + 0.5) > 4` is `sum(x) > 4 - 0.5 * n`), but a helper holds the whole
	// value. The message is the binder's (ValidateDecideIntegralComparisonOperands).
	const bool whole = DecideValueIsWholeNumbered(agg, decide.decide_index);
	if (!whole &&
	    (comparison == ExpressionType::COMPARE_LESSTHAN || comparison == ExpressionType::COMPARE_GREATERTHAN ||
	     comparison == ExpressionType::COMPARE_NOTEQUAL)) {
		const bool not_equal = comparison == ExpressionType::COMPARE_NOTEQUAL;
		const bool less = comparison == ExpressionType::COMPARE_LESSTHAN;
		throw BinderException("Comparison '%s' is not supported here: '%s' can take fractional values in this "
		                      "constraint, and %s. Compare with %s instead.",
		                      not_equal ? "<>"
		                      : less    ? "<"
		                                : ">",
		                      RenderDecideSource(agg, names),
		                      not_equal ? "the rewrite (x <> K becomes x <= K-1 OR x >= K+1) is exact only on "
		                                  "whole numbers"
		                                : "stepping the bound is exact only on whole numbers",
		                      not_equal ? "'<=' or '>='"
		                      : less    ? "'<='"
		                                : "'>='");
	}

	idx_t link_idx = helpers.Find(agg, scope, source_clause_id, objective_row);
	if (link_idx != DConstants::INVALID_INDEX) {
		auto &link = decide.reducer_value_links[link_idx];
		link.need_envelope = link.need_envelope || need_envelope;
		link.need_closing = link.need_closing || need_closing;
		*site.slot = MakeDecisionRef(decide, link.value_var);
		return;
	}

	LogicalDecide::ReducerValueLink link;
	link.agg_name = agg_name;
	link.label = label;
	TryParseGroupKeyTag(agg.alias, link.group_key);
	TryParseQualifiedReducerTag(agg.alias, link.qualifier_scope_idx);
	link.inner_expr = agg.children[0]->Copy();
	if (agg.filter) {
		link.filter = agg.filter->Copy();
	}
	if (scope.when) {
		link.when = scope.when->Copy();
	}
	link.source_clause_id = source_clause_id;
	link.removal_group_id = removal_group_id;
	link.need_envelope = need_envelope;
	link.need_closing = need_closing;
	link_idx = decide.reducer_value_links.size();

	// The helper decision: whole-numbered when the reducer's value is, so a strict bound on
	// the rewritten row still steps on the integer lattice; a query-wide decision for `by ()`,
	// else one instance per group of the `by` key among the rows the clause keeps -- a
	// scope of its own (EntityScopeInfo::value_link), which execution builds from the link.
	auto var_scope = DecideVarScopeInfo::Scalar();
	if (link.group_key != DConstants::INVALID_INDEX) {
		EntityScopeInfo helper_scope;
		helper_scope.table_alias = decide.group_keys[link.group_key].text;
		helper_scope.key_names = decide.group_keys[link.group_key].names;
		helper_scope.value_link = link_idx;
		decide.entity_scopes.push_back(std::move(helper_scope));
		var_scope = DecideVarScopeInfo::Entity(decide.entity_scopes.size() - 1);
	}
	LogicalType aux_type = whole ? LogicalType::INTEGER : LogicalType::DOUBLE;
	// F6: a diagnosis, and a plan, name the helper by the reducer it holds, `by` included.
	auto plan_label = RenderDecideSource(agg, names);
	link.value_var =
	    AddAuxiliaryDecision(decide, "__value_" + to_string(link_idx) + "__", aux_type, false, var_scope, plan_label);
	AddRewriteAccount(decide, source_clause_id,
	                  var_scope.IsEntity() ? LogicalDecide::RewriteAccount::Kind::VALUE_HELPER_PER_GROUP
	                                       : LogicalDecide::RewriteAccount::Kind::VALUE_HELPER,
	                  plan_label);
	if (var_scope.IsEntity()) {
		decide.entity_scopes[var_scope.entity_scope_idx].scoped_variable_indices.push_back(link.value_var);
	}
	helpers.reducers.push_back(agg.Copy());
	helpers.objective.push_back(objective_row);
	*site.slot = MakeDecisionRef(decide, link.value_var);
	decide.reducer_value_links.push_back(std::move(link));
}

} // namespace

void DecideOptimizer::RewriteScopedReducers(LogicalDecide &decide) {
	if (!decide.decide_constraints) {
		return;
	}
	auto names = DecidePlanNames(decide.source_fragments, decide.entity_scopes, decide.group_keys, decide.decide_index,
	                             decide.aux_var_expressions);
	Helpers helpers {decide, {}, {}};

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
		CollectReducerSites(cmp.left, 1, optimizer.context, decide.decide_index, sites);
		const bool objective_row = HasDecideTag(cmp.GetAlias(), OBJECTIVE_ROW_TAG);
		for (auto &site : sites) {
			ReplaceReducer(helpers, names, scope, cmp.type, source_clause_id, removal_group_id, objective_row, site);
		}
		// One row per class from here on: the shape tag has done its work.
		auto alias = cmp.GetAlias();
		RemoveDecideTag(alias, VALUE_REWRITE_TAG);
		cmp.SetAlias(std::move(alias));
	};
	rewrite(decide.decide_constraints, ClauseScope());
}

} // namespace duckdb
