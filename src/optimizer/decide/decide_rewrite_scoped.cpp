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
	//! A decision multiplies the reducer (`b * sum(x)`): its product with the helper is the Boolean product.
	bool under_decision;
};

void CollectReducerSites(unique_ptr<Expression> &expr, int sign, ClientContext &context, idx_t decide_index,
                         const BooleanDecisions &booleans, vector<ReducerSite> &out, bool under_decision = false) {
	if (!BoundExpressionReferencesDecide(*expr, decide_index)) {
		return; // data holds no decision reducer
	}
	switch (expr->GetExpressionClass()) {
	case ExpressionClass::BOUND_AGGREGATE:
		out.push_back({&expr, sign, under_decision});
		return;
	case ExpressionClass::BOUND_CAST:
		CollectReducerSites(expr->Cast<BoundCastExpression>().child, sign, context, decide_index, booleans, out,
		                    under_decision);
		return;
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr->Cast<BoundFunctionExpression>();
		auto &name = func.function.name;
		if (name == "+") {
			for (auto &child : func.children) {
				CollectReducerSites(child, sign, context, decide_index, booleans, out, under_decision);
			}
			return;
		}
		if (name == "-" && func.children.size() == 2) {
			CollectReducerSites(func.children[0], sign, context, decide_index, booleans, out, under_decision);
			CollectReducerSites(func.children[1], -sign, context, decide_index, booleans, out, under_decision);
			return;
		}
		if (name == "-" && func.children.size() == 1) {
			CollectReducerSites(func.children[0], -sign, context, decide_index, booleans, out, under_decision);
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
				// A BOOL decision is never negative, so it keeps the direction of the reducer it scales.
				int factor_sign = 0;
				if (!(name == "/" && i == 1)) {
					factor_sign = ScaleSignAtPlanTime(context, other.get(), name == "/", &booleans);
				}
				CollectReducerSites(term, sign * factor_sign, context, decide_index, booleans, out,
				                    under_decision || BoundExpressionReferencesDecide(*other, decide_index));
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
		CollectReducerSites(child, 0, context, decide_index, booleans, out, under_decision);
	});
}

//! The helper decisions allocated so far, each with the reducer it holds as written (the
//! alias carries its `by` key and qualifier). One reducer over one set of rows is one
//! helper, however many rows read it: an ABS splits a clause into an envelope pair, each
//! row holding a copy of the reducer, and two helpers pinned on different sides would let
//! the pair drift apart; two clauses holding the same reducer (`x <= 0.5 * sum(x) AND
//! x >= 0.1 * sum(x)`) say the same thing twice. The definition belongs to no clause
//! (owner ruling B10): it is part of the fixed model, so a DROP of a clause that reads the
//! helper leaves it, and an equality on a helper nothing else reads always holds.
struct Helpers {
	ClientContext &context;
	LogicalDecide &decide;
	vector<unique_ptr<Expression>> reducers; //!< parallel to decide.reducer_value_links

	//! The link holding `agg` over the rows `scope` keeps, or INVALID_INDEX.
	idx_t Find(const BoundAggregateExpression &agg, const DecideEnclosingScope &scope) const {
		auto when = scope.Condition();
		for (idx_t i = 0; i < reducers.size(); i++) {
			auto &link = decide.reducer_value_links[i];
			if (!reducers[i]->Equals(agg) || reducers[i]->GetAlias() != agg.GetAlias()) {
				continue;
			}
			// A clause's rows carry copies of its `when`: the same condition.
			const bool same_when = when ? link.when && link.when->Equals(*when) : !link.when;
			if (same_when) {
				return i;
			}
		}
		return DConstants::INVALID_INDEX;
	}
};

//! `<`, `>` and `<>` step onto the whole numbers, which is exact for the row only when the helper is whole. The
//! binder admitted the clause by its own reading: a constant beside the decisions inside a SUM moves to the bound when
//! the reducer is summed in place (`sum(x + 0.5) > 4` is `sum(x) > 4 - 0.5 * n`), but a helper holds the whole value.
//! The message is the binder's (ValidateDecideIntegralComparisonOperands).
void RefuseFractionalStep(const BoundAggregateExpression &agg, const DecideSourceNames &names, idx_t decide_index,
                          ExpressionType comparison) {
	if (DecideValueIsWholeNumbered(agg, decide_index) ||
	    !(comparison == ExpressionType::COMPARE_LESSTHAN || comparison == ExpressionType::COMPARE_GREATERTHAN ||
	      comparison == ExpressionType::COMPARE_NOTEQUAL)) {
		return;
	}
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

//! Replace the reducer at `site` by its helper decision: the clause's existing one for a
//! repeated occurrence, else a new one, recording what it holds. `outer` is the push of the side the reducer
//! stands in: the comparison's, or the objective's sense.
void ReplaceReducer(Helpers &helpers, const DecideSourceNames &names, const DecideEnclosingScope &scope,
                    ExtremumPush outer, idx_t source_clause_id, const ReducerSite &site) {
	auto &decide = helpers.decide;
	auto &agg = (*site.slot)->Cast<BoundAggregateExpression>();
	auto agg_name = StringUtil::Lower(agg.function.name);
	auto label = RenderDecideReducer(agg, names);
	// Which side pins the helper is the one the row would otherwise let it escape through
	// (the epigraph form; the Plaisted-Greenbaum polarity of DefineNonlinearReducerBodies). Under
	// `<=` a positive term is pushed down, so a MAX needs its envelope (`value >= member`) and a
	// MIN its closing side (`value >= some member`, with indicators); a SUM or AVG, which like a
	// MAX never decreases when a member grows, needs `value >= sum`, its envelope. The mirror
	// holds for `>=` and for a negative term. An unknown sign, `=` and `<>` need both. A helper
	// several rows read needs every side any of them does, and the rows read the helper alone:
	// a slack value is harmless.
	const ExtremumPush push = ExtremumPushOf(outer, site.sign);
	const bool easy = IsEasyExtremum(agg_name != "min", push);
	const bool need_envelope = push == ExtremumPush::NONE || easy;
	const bool need_closing = push == ExtremumPush::NONE || !easy;

	const bool whole = DecideValueIsWholeNumbered(agg, decide.decide_index);

	// F6: a diagnosis, and a plan, name the helper by the reducer it holds, `by` included.
	auto plan_label = RenderDecideSource(agg, names);
	idx_t group_key = DConstants::INVALID_INDEX;
	TryParseGroupKeyTag(agg.alias, group_key);
	auto account_kind = group_key == DConstants::INVALID_INDEX
	                        ? LogicalDecide::RewriteAccount::Kind::VALUE_HELPER
	                        : LogicalDecide::RewriteAccount::Kind::VALUE_HELPER_PER_GROUP;
	idx_t link_idx = helpers.Find(agg, scope);
	if (link_idx != DConstants::INVALID_INDEX) {
		auto &link = decide.reducer_value_links[link_idx];
		link.need_envelope = link.need_envelope || need_envelope;
		link.need_closing = link.need_closing || need_closing;
		// Each clause that reads the helper says so in its own plan, once.
		bool told = false;
		for (auto &account : decide.rewrite_accounts) {
			told = told || (account.source_clause_id == source_clause_id && account.kind == account_kind &&
			                account.subject == plan_label);
		}
		if (!told) {
			AddRewriteAccount(decide, source_clause_id, account_kind, plan_label);
		}
		ReplaceWithDecision(helpers.context, decide, link.value_var, *site.slot);
		return;
	}

	LogicalDecide::ReducerValueLink link;
	link.agg_name = agg_name;
	link.label = label;
	link.group_key = group_key;
	TryParseQualifiedReducerTag(agg.alias, link.qualifier_scope_idx);
	link.inner_expr = agg.children[0]->Copy();
	if (agg.filter) {
		link.filter = agg.filter->Copy();
	}
	if (scope.Condition()) {
		link.when = scope.Condition()->Copy();
	}
	link.need_envelope = need_envelope;
	link.need_closing = need_closing;
	link_idx = decide.reducer_value_links.size();

	// The helper decision: whole-numbered when the reducer's value is, so a strict bound on
	// the rewritten row still steps on the integer lattice; a query-wide decision for `by ()`,
	// else one instance per group of the `by` key among the rows the clause keeps -- a
	// scope of its own (KeyedScopeInfo::value_link), which execution builds from the link.
	auto var_scope = DecideVarScopeInfo::Global();
	if (link.group_key != DConstants::INVALID_INDEX) {
		KeyedScopeInfo helper_scope;
		helper_scope.key = link.group_key;
		helper_scope.value_link = link_idx;
		decide.keyed_scopes.push_back(std::move(helper_scope));
		var_scope = DecideVarScopeInfo::Keyed(decide.keyed_scopes.size() - 1);
	}
	LogicalType aux_type = whole ? LogicalType::INTEGER : LogicalType::DOUBLE;
	link.value_var =
	    AddAuxiliaryDecision(decide, "__value_" + to_string(link_idx) + "__", aux_type, false, var_scope, plan_label);
	AddRewriteAccount(decide, source_clause_id, account_kind, plan_label);
	if (var_scope.IsKeyed()) {
		decide.keyed_scopes[var_scope.keyed_scope_idx].scoped_variable_indices.push_back(link.value_var);
	}
	// A reducer's value may be negative: no default floor. The formulation pass boxes it by its members' reach.
	decide.signed_helper_vars.push_back(link.value_var);
	helpers.reducers.push_back(agg.Copy());
	ReplaceWithDecision(helpers.context, decide, link.value_var, *site.slot);
	decide.reducer_value_links.push_back(std::move(link));
}

} // namespace

void DecideOptimizer::RewriteScopedReducers(LogicalDecide &decide) {
	if (!decide.decide_constraints && !decide.decide_objective) {
		return;
	}
	const BooleanDecisions booleans {decide.decide_index, decide.is_boolean_var};
	auto names = DecidePlanNames(decide.source_fragments, decide.keyed_scopes, decide.keys, decide.decide_index,
	                             decide.aux_var_expressions);
	Helpers helpers {optimizer.context, decide, {}};

	// A Boolean factor on a reducer (`b * sum(x)`, canonicalization admits no other decision) is now a product with
	// the helper. The bilinear rewrite ran before this pass, so it never saw the product: hand it to its one-product
	// linearization, the same McCormick column any Boolean product gets.
	auto HandProductsToBilinear = [&](unique_ptr<Expression> &expr, const string &source_alias) {
		vector<LogicalDecide::BilinearLink> products;
		FindAndReplaceBilinear(expr, decide, products, source_alias);
		for (auto &product : products) {
			decide.bilinear_links.push_back(product);
		}
	};

	std::function<void(unique_ptr<Expression> &, DecideEnclosingScope)> rewrite = [&](unique_ptr<Expression> &expr,
	                                                                                  DecideEnclosingScope scope) {
		scope = EnterDecideScope(*expr, scope);
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
		TryParseSourceClauseTag(cmp.GetAlias(), source_clause_id);
		// Canonicalization put every decision reducer on the left.
		vector<ReducerSite> sites;
		CollectReducerSites(cmp.left, 1, optimizer.context, decide.decide_index, booleans, sites);
		for (auto &site : sites) {
			RefuseFractionalStep(site.slot->get()->Cast<BoundAggregateExpression>(), names, decide.decide_index,
			                     cmp.type);
			ReplaceReducer(helpers, names, scope, ExtremumPushOf(cmp.type), source_clause_id, site);
		}
		HandProductsToBilinear(cmp.left, cmp.GetAlias());
		// One row per class from here on: the shape tag has done its work.
		auto alias = cmp.GetAlias();
		RemoveDecideTag(alias, VALUE_REWRITE_TAG);
		cmp.SetAlias(std::move(alias));
	};
	if (decide.decide_constraints) {
		rewrite(decide.decide_constraints, DecideEnclosingScope());
	}

	// The objective is one expression under its sense (`MAXIMIZE b * sum(x)`): a reducer under a Boolean decision
	// takes a helper decision the same way. The push is the sense times the term's sign, as for the body definitions.
	// A reducer no decision multiplies stays in the objective as it is.
	if (decide.decide_objective) {
		auto objective = std::move(decide.decide_objective);
		auto *body = &objective;
		DecideEnclosingScope scope;
		while ((*body)->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conjunction = (*body)->Cast<BoundConjunctionExpression>();
			if (!IsConstraintWrapper(conjunction) || conjunction.children.empty()) {
				break;
			}
			scope = EnterDecideScope(conjunction, scope);
			body = &conjunction.children[0];
		}
		if (HasDecideTag((*body)->GetAlias(), VALUE_REWRITE_TAG)) {
			vector<ReducerSite> sites;
			CollectReducerSites(*body, 1, optimizer.context, decide.decide_index, booleans, sites);
			const auto outer = decide.decide_sense == DecideSense::MINIMIZE ? ExtremumPush::DOWN : ExtremumPush::UP;
			for (auto &site : sites) {
				if (site.under_decision) {
					ReplaceReducer(helpers, names, scope, outer, DConstants::INVALID_INDEX, site);
				}
			}
			HandProductsToBilinear(*body, OBJECTIVE_ROW_TAG);
		}
		decide.SetObjective(optimizer.context, std::move(objective));
	}
}

} // namespace duckdb
