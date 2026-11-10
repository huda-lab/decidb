//===----------------------------------------------------------------------===//
//                         DeciDB
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
// A range frame over decisions (language/guide.md §5.6) is a reducer whose groups are the frame's windows, and becomes
// a helper through the same door (AddValueHelper, DecideHelperSides; RewriteFrameRanges in decide_rewrite_frames.cpp).
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
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

void decide_rewrite::DecideHelperSides(const string &agg_name, ExtremumPush push, bool &need_envelope,
                                       bool &need_closing) {
	// Which side pins the helper is the one the row would otherwise let it escape through (the epigraph form; the
	// Plaisted-Greenbaum polarity of DefineNonlinearReducerBodies). Under `<=` a positive term is pushed down, so a
	// MAX needs its envelope (`value >= member`) and a MIN its closing side (`value >= some member`, with indicators);
	// a SUM or AVG, which like a MAX never decreases when a member grows, needs `value >= sum`, its envelope. The
	// mirror holds for `>=` and for a negative term. An unknown sign, `=` and `<>` need both.
	const bool easy = IsEasyExtremum(agg_name != "min", push);
	need_envelope = push == ExtremumPush::NONE || easy;
	need_closing = push == ExtremumPush::NONE || !easy;
}

idx_t decide_rewrite::AddValueHelper(LogicalDecide &decide, LogicalDecide::ReducerValueLink link,
                                     const KeyedScopeInfo &scope, bool whole, idx_t source_clause_id,
                                     LogicalDecide::RewriteAccount::Kind account) {
	const idx_t link_idx = decide.reducer_value_links.size();
	// One instance per class of a scope of its own (KeyedScopeInfo::value_link), which execution builds from the link.
	KeyedScopeInfo helper_scope = scope;
	helper_scope.value_link = link_idx;
	decide.keyed_scopes.push_back(std::move(helper_scope));
	auto var_scope = DecideVarScopeInfo::Keyed(decide.keyed_scopes.size() - 1);
	// Whole-numbered when the value is, so a strict bound on a row reading it still steps on the integer lattice.
	// F6: a diagnosis, and a plan, name the helper by what it holds.
	LogicalType aux_type = whole ? LogicalType::INTEGER : LogicalType::DOUBLE;
	link.value_var =
	    AddAuxiliaryDecision(decide, "__value_" + to_string(link_idx) + "__", aux_type, false, var_scope, link.label);
	AddRewriteAccount(decide, source_clause_id, account, link.label);
	// A reducer's value may be negative: no default floor. The formulation pass boxes it by its members' reach.
	decide.signed_helper_vars.push_back(link.value_var);
	decide.reducer_value_links.push_back(std::move(link));
	return link_idx;
}

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
	case ExpressionClass::BOUND_AGGREGATE: {
		idx_t frame;
		if (!TryGetDecideFrame(*expr, frame)) {
			out.push_back({&expr, sign, under_decision}); // a frame is a tuple term of the row, no reducer
		}
		return;
	}
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
	vector<unique_ptr<Expression>> reducers; //!< each helper's reducer, as written
	vector<idx_t> links;                     //!< parallel: its link, into decide.reducer_value_links

	//! The link holding `agg` over `rows` grouped by `group_key` -- its path's (language/guide.md §5.5) --, or
	//! INVALID_INDEX. Other links -- a range frame's (RewriteFrameRanges) -- stand among these.
	idx_t Find(const BoundAggregateExpression &agg, const Expression *rows, idx_t group_key) const {
		for (idx_t i = 0; i < reducers.size(); i++) {
			auto &link = decide.reducer_value_links[links[i]];
			if (link.group_key != group_key || !DecideExpressionsEqual(*reducers[i], agg) ||
			    reducers[i]->GetAlias() != agg.GetAlias()) {
				continue;
			}
			// A clause's rows carry copies of its `when`: the same condition.
			const bool same_rows = rows ? link.when && DecideExpressionsEqual(*link.when, *rows) : !link.when;
			if (same_rows) {
				return links[i];
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
//! repeated occurrence, else a new one, recording what it holds. The reducers inside its
//! argument are replaced first, each by a helper keyed by its path (language/guide.md §5.5), so that what the
//! helper holds is affine in helpers, decisions and data. `outer` is the push of the side the reducer stands in: the
//! comparison's, the objective's sense, or for a nested one its reader's.
void ReplaceReducer(Helpers &helpers, const DecideSourceNames &names, const BooleanDecisions &booleans,
                    const DecideReducerPath &path, ExtremumPush outer, idx_t source_clause_id,
                    const ReducerSite &site) {
	auto &decide = helpers.decide;
	auto &agg = (*site.slot)->Cast<BoundAggregateExpression>();
	auto agg_name = StringUtil::Lower(agg.function.name);
	// F6: a diagnosis, and a plan, name the helper by the reducer it holds, `by` included, as written: read before the
	// reducers inside it become helpers.
	auto plan_label = RenderDecideSource(agg, names);
	// Which side pins the helper is the one the row would otherwise let it escape through (DecideHelperSides). A
	// helper several rows read needs every side any of them does, and the rows read the helper alone: a slack value
	// is harmless.
	const ExtremumPush push = ExtremumPushOf(outer, site.sign);
	bool need_envelope, need_closing;
	DecideHelperSides(agg_name, push, need_envelope, need_closing);

	// A member is pushed as the helper is, through the sign it carries in the argument: a SUM or AVG is linear in it,
	// and a MIN or MAX does not decrease when it grows, so either passes its push on (D79).
	vector<ReducerSite> nested;
	if (!agg.children.empty()) {
		CollectReducerSites(agg.children[0], 1, helpers.context, decide.decide_index, booleans, nested);
	}
	auto below = path.Below(agg);
	for (auto &inner : nested) {
		// The binder refuses a decision times a reducer inside a reducer (D80).
		D_ASSERT(!inner.under_decision);
		ReplaceReducer(helpers, names, booleans, below, push, source_clause_id, inner);
	}
	// Among nested reducers every one is a helper, so none is summed in place: a square or product of decisions in
	// its body would make the helper's definition quadratic (D80). DefineNonlinearReducerBodies has named such a
	// body by now wherever a constraint, or a MIN or MAX of the objective, reads it.
	if ((!path.keys.empty() || !nested.empty()) &&
	    !DecideExpressionDegree(*agg.children[0], decide.decide_index).IsLinear()) {
		throw BinderException("DECIDE: '%s' squares or multiplies decisions inside nested reducers, which is not "
		                      "supported yet",
		                      plan_label);
	}

	// Read once the reducers inside it are helpers, each whole-numbered when its value is.
	const bool whole = DecideValueIsWholeNumbered(agg, decide.decide_index);

	// It reads the rows its path keeps, grouped by its path key: for a reducer at the top, the clause's kept rows by
	// its own `by` key.
	auto rows = path.Rows();
	const idx_t group_key = FindOrCreateDecidePathKey(path.KeysOf(DecideReducerGroupKey(agg)), decide.keys);
	auto account_kind = group_key == DConstants::INVALID_INDEX
	                        ? LogicalDecide::RewriteAccount::Kind::VALUE_HELPER
	                        : LogicalDecide::RewriteAccount::Kind::VALUE_HELPER_PER_GROUP;
	idx_t link_idx = helpers.Find(agg, rows.get(), group_key);
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
	// Its columns are named by the reducer it holds, `by` included: a refusal's remedy bounds that (F6).
	link.label = plan_label;
	link.group_key = group_key;
	TryParseQualifiedReducerTag(agg.alias, link.qualifier_scope_idx);
	link.inner_expr = agg.children[0]->Copy();
	if (agg.filter) {
		link.filter = agg.filter->Copy();
	}
	link.when = std::move(rows);
	link.need_envelope = need_envelope;
	link.need_closing = need_closing;

	// The helper decision: one instance per group of its path key among the rows its path keeps -- `by ()`
	// throughout is one group, or none when no row holds a member.
	KeyedScopeInfo scope;
	scope.key = link.group_key;
	link_idx = AddValueHelper(decide, std::move(link), scope, whole, source_clause_id, account_kind);
	helpers.reducers.push_back(agg.Copy());
	helpers.links.push_back(link_idx);
	ReplaceWithDecision(helpers.context, decide, decide.reducer_value_links[link_idx].value_var, *site.slot);
}

//! `sum([when c] per K: inner([when c'] e) by (G))` in the objective, `inner` a SUM or AVG with no `per` and no
//! reducer inside, every element of K in G: each G group is one member, so the sum of the members is `inner` read
//! over every group, `c` joined to `c'`. Written so, the objective sums -- or averages per G group -- `e` itself, a
//! square included, which a helper could not define linearly (D81). Replaces the reducer at `slot` and returns true;
//! false when it is not that shape.
bool InlineSumOfGroupSums(LogicalDecide &decide, ClientContext &context, unique_ptr<Expression> &slot) {
	auto &outer = slot->Cast<BoundAggregateExpression>();
	idx_t qualifier;
	if (!StringUtil::CIEquals(outer.function.name, "sum") || outer.children.size() != 1 ||
	    DecideReducerGroupKey(outer) != DConstants::INVALID_INDEX ||
	    !TryParseQualifiedReducerTag(outer.GetAlias(), qualifier) || qualifier >= decide.keyed_scopes.size()) {
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
	auto &by = DecideKeyElements(decide.keys, DecideReducerGroupKey(inner));
	for (auto &element : DecideKeyElements(decide.keys, decide.keyed_scopes[qualifier].key)) {
		bool in_by = false;
		for (auto &part : by) {
			in_by = in_by || part.Grouping().Equals(element.Grouping());
		}
		if (!in_by) {
			return false; // a G group holding two classes of K would be counted twice
		}
	}
	auto inlined = inner.Copy();
	const auto retained = decide.retained_expressions.size();
	decide.retained_expressions.push_back(outer.Copy());
	AddDecideTag(inlined->alias, string(TREE_RETAINED_EXPRESSION_TAG_PREFIX) + to_string(retained) + "__");
	auto &reducer = inlined->Cast<BoundAggregateExpression>();
	auto add_condition = [&](unique_ptr<Expression> condition) {
		if (!reducer.filter) {
			reducer.filter = std::move(condition);
			return;
		}
		auto both = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		both->children.push_back(std::move(condition));
		both->children.push_back(std::move(reducer.filter));
		reducer.filter = std::move(both);
	};
	if (outer.filter) {
		add_condition(outer.filter->Copy());
	}
	// A row in no G group -- a NULL in an element that groups NULL as no group -- is a member of no group, so the
	// sum read whole skips it as the groups did (D5).
	for (auto &part : by) {
		if (part.null_mode != DecideKeyNullMode::NO_GROUP) {
			continue;
		}
		auto test = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
		test->children.push_back(part.expression->Copy());
		test->SetAlias(SKIP_NULL_TAG);
		add_condition(std::move(test));
	}
	slot = BoundCastExpression::AddCastToType(context, std::move(inlined), outer.return_type);
	return true;
}

} // namespace

void DecideOptimizer::SwapObjectiveSummation(LogicalDecide &decide) {
	if (!decide.decide_objective) {
		return;
	}
	const idx_t decide_index = decide.decide_index;
	auto real = [&](unique_ptr<Expression> expr) {
		return BoundCastExpression::AddCastToType(optimizer.context, std::move(expr), LogicalType::DOUBLE);
	};
	auto number = [](double value) {
		return make_uniq<BoundConstantExpression>(Value::DOUBLE(value));
	};
	// `outer` reads `inner(e) by (G)` once per row it keeps, so each G group once per such row of it: summed in the
	// other order, it is one sum over those rows of `e` times `weight`, the group's rows the outer keeps -- divided,
	// for an AVG, by the group's own members -- and 0 where `inner`'s own `when` drops the row. A square of a linear
	// form is weighted inside, by the root, since a weight is never negative: the objective stays a sum of squares.
	auto swap = [&](unique_ptr<Expression> &slot) {
		auto &outer = slot->Cast<BoundAggregateExpression>();
		idx_t frame, scope;
		if (TryGetDecideFrame(outer, frame) || !StringUtil::CIEquals(outer.function.name, "sum") ||
		    outer.children.size() != 1 || TryParseQualifiedReducerTag(outer.GetAlias(), scope)) {
			return;
		}
		auto *body = UnwrapDecideCasts(*outer.children[0], decide_index);
		if (body->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return;
		}
		auto &inner = body->Cast<BoundAggregateExpression>();
		const bool avg = StringUtil::CIEquals(inner.function.name, "avg");
		if ((!avg && !StringUtil::CIEquals(inner.function.name, "sum")) || inner.children.size() != 1 ||
		    TryGetDecideFrame(inner, frame) || TryParseQualifiedReducerTag(inner.GetAlias(), scope) ||
		    DecideCanonicalizer::ContainsReducer(*inner.children[0]) || ContainsDecideFrame(*inner.children[0]) ||
		    (inner.filter && ContainsDecideFrame(*inner.filter)) ||
		    DecideExpressionDegree(*inner.children[0], decide_index).IsLinear()) {
			return; // a linear nest is read through helpers, which the model writes in (D89)
		}
		const idx_t group_key = DecideReducerGroupKey(inner);
		auto group_count = [&](unique_ptr<Expression> summand) {
			vector<unique_ptr<Expression>> children;
			children.push_back(std::move(summand));
			auto count = optimizer.BindAggregateFunction("sum", std::move(children));
			if (group_key != DConstants::INVALID_INDEX) {
				count->SetAlias(MakeGroupKeyTag(group_key));
			}
			return real(std::move(count));
		};
		auto member = [&]() -> unique_ptr<Expression> {
			if (!inner.filter) {
				return number(1.0);
			}
			return make_uniq<BoundCaseExpression>(inner.filter->Copy(), number(1.0), number(0.0));
		};
		unique_ptr<Expression> weight = group_count(number(1.0));
		if (avg) {
			weight = optimizer.BindScalarFunction("/", std::move(weight), group_count(member()));
		}
		if (inner.filter) {
			weight = make_uniq<BoundCaseExpression>(inner.filter->Copy(), std::move(weight), number(0.0));
		}
		// A row in no G group (a NULL in a key that groups NULL as none, D5) is read by no group: weight 0.
		auto weighted = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_COALESCE, LogicalType::DOUBLE);
		weighted->children.push_back(std::move(weight));
		weighted->children.push_back(number(0.0));
		const Expression &w = *weighted;

		std::function<unique_ptr<Expression>(const Expression &)> weigh = [&](const Expression &term) {
			auto *cur = UnwrapDecideCasts(term, decide_index);
			if (cur->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
			    BoundExpressionReferencesDecide(*cur, decide_index)) {
				auto &func = cur->Cast<BoundFunctionExpression>();
				auto name = StringUtil::Lower(func.function.name);
				if ((name == "+" || name == "-") && func.children.size() == 2) {
					return optimizer.BindScalarFunction(name, weigh(*func.children[0]), weigh(*func.children[1]));
				}
				if (name == "-" && func.children.size() == 1) {
					return optimizer.BindScalarFunction("-", weigh(*func.children[0]));
				}
				if (name == "*" && func.children.size() == 2) {
					for (idx_t side = 0; side < 2; side++) {
						if (!BoundExpressionReferencesDecide(*func.children[side], decide_index)) {
							return optimizer.BindScalarFunction("*", real(func.children[side]->Copy()),
							                                    weigh(*func.children[1 - side]));
						}
					}
				}
				const Expression *square = nullptr;
				if ((name == "power" || name == "pow" || name == "**") && func.children.size() == 2) {
					double exponent;
					if (TryEvaluateFoldableDouble(optimizer.context, *func.children[1], exponent, FOLD_PLAIN) &&
					    exponent == 2.0) {
						square = func.children[0].get();
					}
				} else if (name == "*" && func.children.size() == 2 && func.children[0]->Equals(*func.children[1])) {
					square = func.children[0].get();
				}
				if (square && DecideExpressionDegree(*square, decide_index).IsLinear()) {
					auto root = optimizer.BindScalarFunction("sqrt", w.Copy());
					auto scaled = optimizer.BindScalarFunction("*", std::move(root), real(square->Copy()));
					return optimizer.BindScalarFunction("power", std::move(scaled), number(2.0));
				}
			}
			return optimizer.BindScalarFunction("*", w.Copy(), real(term.Copy()));
		};
		vector<unique_ptr<Expression>> children;
		children.push_back(weigh(*inner.children[0]));
		auto swapped = optimizer.BindAggregateFunction("sum", std::move(children));
		auto &reducer = swapped->Cast<BoundAggregateExpression>();
		if (outer.filter) {
			reducer.filter = outer.filter->Copy();
		}
		auto alias = outer.GetAlias();
		RemoveDecideTag(alias, VALUE_REWRITE_TAG); // it holds no reducer over decisions now
		reducer.SetAlias(std::move(alias));
		slot = BoundCastExpression::AddCastToType(optimizer.context, std::move(swapped), outer.return_type);
	};
	std::function<void(unique_ptr<Expression> &)> visit = [&](unique_ptr<Expression> &expr) {
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			swap(expr);
			return;
		}
		ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { visit(child); });
	};
	visit(decide.decide_objective);
}

void DecideOptimizer::LiftNestedReducers(LogicalDecide &decide) {
	if (!decide.decide_constraints && !decide.decide_objective) {
		return;
	}
	const BooleanDecisions booleans {decide.decide_index, decide.is_boolean_var};
	auto names = DecidePlanNames(decide.source_fragments, decide.keyed_scopes, decide.keys, decide.decide_index,
	                             decide.aux_var_expressions);
	Helpers helpers {optimizer.context, decide, {}, {}};
	auto reads_decision = [&](const Expression &expr) {
		return BoundExpressionReferencesDecide(expr, decide.decide_index);
	};
	// `expr` stands in the argument of a reducer read along `path` when `nested`; `affine` while every node between
	// them is `+`, `-`, a cast or a data factor.
	std::function<void(unique_ptr<Expression> &, const DecideReducerPath &, bool, bool, idx_t)> walk =
	    [&](unique_ptr<Expression> &expr, const DecideReducerPath &path, bool nested, bool affine, idx_t clause) {
		    if (!reads_decision(*expr)) {
			    return;
		    }
		    if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			    idx_t frame;
			    if (TryGetDecideFrame(*expr, frame)) {
				    return; // a frame is a tuple term of the row
			    }
			    if (nested && !affine) {
				    ReducerSite site {&expr, 0, false};
				    ReplaceReducer(helpers, names, booleans, path, ExtremumPush::NONE, clause, site);
				    return;
			    }
			    auto &agg = expr->Cast<BoundAggregateExpression>();
			    auto below = path.Below(agg);
			    for (auto &child : agg.children) {
				    walk(child, below, true, true, clause);
			    }
			    return;
		    }
		    bool keeps = expr->GetExpressionClass() == ExpressionClass::BOUND_CAST;
		    if (expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			    auto &func = expr->Cast<BoundFunctionExpression>();
			    auto &name = func.function.name;
			    if (name == "+" || name == "-") {
				    keeps = true;
			    } else if ((name == "*" || name == "/") && func.children.size() == 2) {
				    // A data factor scales what it multiplies; a decision on both sides, or in a divisor, does not.
				    keeps = !(reads_decision(*func.children[0]) && reads_decision(*func.children[1])) &&
				            !(name == "/" && reads_decision(*func.children[1]));
			    }
		    }
		    ExpressionIterator::EnumerateChildren(
		        *expr, [&](unique_ptr<Expression> &child) { walk(child, path, nested, affine && keeps, clause); });
	    };

	std::function<void(unique_ptr<Expression> &, DecideEnclosingScope)> clauses = [&](unique_ptr<Expression> &expr,
	                                                                                  DecideEnclosingScope scope) {
		scope = EnterDecideScope(*expr, scope);
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conj = expr->Cast<BoundConjunctionExpression>();
			for (idx_t i = 0; i < conj.children.size(); i++) {
				if (IsConstraintChild(conj, i)) {
					clauses(conj.children[i], scope);
				}
			}
			return;
		}
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return;
		}
		auto &cmp = expr->Cast<BoundComparisonExpression>();
		idx_t source_clause_id = DConstants::INVALID_INDEX;
		TryParseSourceClauseTag(cmp.GetAlias(), source_clause_id);
		DecideReducerPath path;
		if (scope.Condition()) {
			path.conditions.push_back(scope.Condition());
		}
		walk(cmp.left, path, false, true, source_clause_id);
		walk(cmp.right, path, false, true, source_clause_id);
	};
	if (decide.decide_constraints) {
		clauses(decide.decide_constraints, DecideEnclosingScope());
	}
	if (decide.decide_objective) {
		auto *body = &decide.decide_objective;
		DecideEnclosingScope scope;
		while ((*body)->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conjunction = (*body)->Cast<BoundConjunctionExpression>();
			if (!IsConstraintWrapper(conjunction) || conjunction.children.empty()) {
				break;
			}
			scope = EnterDecideScope(conjunction, scope);
			body = &conjunction.children[0];
		}
		DecideReducerPath path;
		if (scope.Condition()) {
			path.conditions.push_back(scope.Condition());
		}
		walk(*body, path, false, true, DConstants::INVALID_INDEX);
	}
}

void DecideOptimizer::RewriteScopedReducers(LogicalDecide &decide) {
	if (!decide.decide_constraints && !decide.decide_objective) {
		return;
	}
	const BooleanDecisions booleans {decide.decide_index, decide.is_boolean_var};
	auto names = DecidePlanNames(decide.source_fragments, decide.keyed_scopes, decide.keys, decide.decide_index,
	                             decide.aux_var_expressions);
	Helpers helpers {optimizer.context, decide, {}, {}};

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
		DecideReducerPath path;
		if (scope.Condition()) {
			path.conditions.push_back(scope.Condition());
		}
		for (auto &site : sites) {
			RefuseFractionalStep(site.slot->get()->Cast<BoundAggregateExpression>(), names, decide.decide_index,
			                     cmp.type);
			ReplaceReducer(helpers, names, booleans, path, ExtremumPushOf(cmp.type), source_clause_id, site);
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
	// takes a helper decision the same way, and so does a reducer that holds a reducer, the canonicalizer's tag on it
	// (D81), unless it is a sum of group sums read in place; every reducer inside it takes one keyed by its path. The
	// push is the sense times the term's sign, as for the body definitions. A reducer no decision multiplies and none
	// nests in stays in the objective as it is.
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
		const bool products = HasDecideTag((*body)->GetAlias(), VALUE_REWRITE_TAG);
		vector<ReducerSite> sites;
		CollectReducerSites(*body, 1, optimizer.context, decide.decide_index, booleans, sites);
		DecideReducerPath path;
		if (scope.Condition()) {
			path.conditions.push_back(scope.Condition());
		}
		const auto outer = decide.decide_sense == DecideSense::MINIMIZE ? ExtremumPush::DOWN : ExtremumPush::UP;
		for (auto &site : sites) {
			const bool nest = HasDecideTag((*site.slot)->GetAlias(), VALUE_REWRITE_TAG);
			if (nest && !site.under_decision && InlineSumOfGroupSums(decide, optimizer.context, *site.slot)) {
				continue;
			}
			if (nest || (products && site.under_decision)) {
				ReplaceReducer(helpers, names, booleans, path, outer, DConstants::INVALID_INDEX, site);
			}
		}
		if (products) {
			HandProductsToBilinear(*body, OBJECTIVE_ROW_TAG);
		}
		decide.SetObjective(optimizer.context, std::move(objective));
	}
}

} // namespace duckdb
