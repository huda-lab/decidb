//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/optimizer/decide/decide_rewrite_scoped.cpp
//
// The value rewrite (language/guide.md §6.4; 05_optimizer/done.md, "The value
// rewrite"). A scoped comparison in the VALUE shape -- a decision reducer beside a
// per-row or keyed decision, under a factor that varies by row, next to a reducer with
// another `by` key, or inside another expression -- is not one model row per `by` group.
// Its decision reducers become helper decisions with one instance per `by` group,
// defined once (LogicalDecide::helper_definitions; stage 06 emits the definition),
// except a direct, unfiltered nested SUM whose scope matches its clause. That shape
// keeps the outer G in the clause and lets its parent read the nested G value directly.
// Deck example 1, `ship <= 0.20 * sum(ship)`, costs N rows and one helper rather than
// N rows of N terms.
//
// A range frame over decisions (language/guide.md §5.6) is a reducer whose groups are the frame's windows, and becomes
// a helper through the same door (AddValueHelper; RewriteFrameRanges in decide_rewrite_frames.cpp).
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
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/decide/decide_shape_check.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/decide/decide_product_form.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

idx_t decide_rewrite::AddValueHelper(LogicalDecide &decide, unique_ptr<DecideHelperDefinition> link,
                                     const KeyedScopeInfo &scope, bool whole, idx_t source_clause_id,
                                     LogicalDecide::RewriteAccount::Kind account) {
	const idx_t link_idx = decide.helper_definitions.size();
	link->origin = DecideRowOrigin::OfClause(source_clause_id);
	// One instance per class of a scope of its own (KeyedScopeInfo::value_link), which execution builds from the link.
	KeyedScopeInfo helper_scope = scope;
	helper_scope.value_link = link_idx;
	decide.bound.keyed_scopes.push_back(std::move(helper_scope));
	auto var_scope = DecideVarScopeInfo::Keyed(decide.bound.keyed_scopes.size() - 1);
	// Whole-numbered when the value is, so a strict bound on a row reading it still steps on the integer lattice.
	// F6: a diagnosis, and a plan, name the helper by what it holds.
	LogicalType aux_type = whole ? LogicalType::INTEGER : LogicalType::DOUBLE;
	const auto name =
	    string(VALUE_LINK_NAME_PREFIX) + to_string(decide.HelperCount(DecideHelperKind::REDUCER_VALUE)) + "__";
	link->helper = AddAuxiliaryDecision(decide, name, aux_type, false, var_scope, link->label);
	AddRewriteAccount(decide, source_clause_id, account, link->label);
	// A reducer's value may be negative: no default floor. The formulation pass boxes it by its members' reach.
	decide.signed_helper_vars.push_back(link->helper);
	decide.helper_definitions.push_back(std::move(link));
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
	//! A function that is no sum or product holds the reducer (`abs(sum(x) - 2)`): the function reads its value, so
	//! the value has to be a column of its own.
	bool under_function;

	bool IsExtremum() const {
		return IsExtremumName(StringUtil::Lower((*slot)->Cast<BoundDecideReducerExpression>().function->name));
	}
};

//! Whether a reducer over decisions stands at or below `expr`.
bool HoldsDecisionReducer(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER) {
		return BoundExpressionReferencesDecide(expr, decide_index);
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { found = found || HoldsDecisionReducer(child, decide_index); });
	return found;
}

//! The decision reducers `expr` holds, each with its sign (WalkPolarity), `sign` being the sign of `expr` itself. A
//! reducer beside another one (`sum(x) * sum(y)`) has no sign of its own to read, as one under a factor that is a
//! data column has none: 0.
vector<ReducerSite> CollectReducerSites(ClientContext &context, const BooleanDecisions &booleans,
                                        unique_ptr<Expression> &expr, int sign = 1) {
	vector<ReducerSite> sites;
	PolarityHolds holds = [&](const Expression &node) {
		return HoldsDecisionReducer(node, booleans.decide_index);
	};
	PolarityVisit visit = [&](unique_ptr<Expression> &node, int node_sign, const PolarityPath &path) {
		if (node->GetExpressionClass() != ExpressionClass::BOUND_DECIDE_REDUCER) {
			return false;
		}
		idx_t frame;
		if (!TryGetDecideFrame(*node, frame)) { // a frame is a tuple term of the row
			sites.push_back({&node, path.beside_held ? 0 : node_sign, path.beside_decision, path.inside_function});
		}
		return true;
	};
	WalkPolarity(context, booleans, expr, sign, holds, visit);
	return sites;
}

//! The pass's access to the helper decisions made so far (LogicalDecide::helper_definitions), each holding its
//! reducer as written. One reducer over one set of rows is one helper, however many rows read it: an ABS splits a
//! clause into an envelope pair, each row holding a copy of the reducer, and two helpers pinned on different sides
//! would let the pair drift apart; two clauses holding the same reducer (`x <= 0.5 * sum(x) AND x >= 0.1 * sum(x)`)
//! say the same thing twice. The definition belongs to no clause (owner ruling B10): it is part of the fixed model,
//! so a DROP of a clause that reads the helper leaves it, and an equality on a helper nothing else reads always
//! holds.
struct Helpers {
	ClientContext &context;
	LogicalDecide &decide;

	//! The class key of a clause: its `per` key, DECIDE_ROW_KEY with no `per`.
	static idx_t ClassKeyOf(const DecideClauseScope &scope) {
		return scope.HasPer() ? scope.class_key : DECIDE_ROW_KEY;
	}

	//! The link holding `agg` as a clause of class key `class_key` reads it over `rows`, grouped by `path_key` -- its
	//! path's (language/guide.md §5.5) --, or INVALID_INDEX. A helper is a reducer's value over the rows its path
	//! keeps, one per group of its path key, so what it stands for is the reducer (DecideExpressionsEqual, which
	//! leaves out its `per` and tells a range frame's marker, RewriteFrameRanges, from a reducer), the key scope of
	//! its own `per`, that path key, the rows and the clause's `per` key. Clauses that agree on all five share one
	//! helper, however they spell the reducer.
	idx_t Find(const BoundDecideReducerExpression &agg, const Expression *rows, idx_t path_key, idx_t class_key) const {
		for (idx_t i = 0; i < decide.helper_definitions.size(); i++) {
			auto &link = *decide.helper_definitions[i];
			if (link.kind != DecideHelperKind::REDUCER_VALUE) {
				continue;
			}
			auto &held = link.Reducer();
			// A clause's rows carry copies of its `when`: the same condition.
			const bool same_rows = rows ? link.when && DecideExpressionsEqual(*link.when, *rows) : !link.when;
			if (DecideExpressionsEqual(held, agg) && held.per == agg.per && link.path_key == path_key && same_rows &&
			    link.class_key == class_key) {
				return i;
			}
		}
		return DConstants::INVALID_INDEX;
	}
};

//! `<`, `>` and `<>` step onto the whole numbers, which is exact for the row only when the helper is whole. The
//! binder admitted the clause by its own reading: a constant beside the decisions inside a SUM moves to the bound when
//! the reducer is summed in place (`sum(x + 0.5) > 4` is `sum(x) > 4 - 0.5 * n`), but a helper holds the whole value.
//! The message is stage 04's (DecideShapeCheck::CheckIntegerStep), raised at `location`: the comparison's,
//! which canonicalization may have rebuilt, else the clause's.
void RefuseFractionalStep(const BoundDecideReducerExpression &agg, const DecideSourceNames &names, idx_t decide_index,
                          const BoundComparisonExpression &comparison, optional_idx location) {
	auto type = comparison.type;
	if (DecideValueIsWholeNumbered(agg, decide_index) ||
	    !(type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_GREATERTHAN ||
	      type == ExpressionType::COMPARE_NOTEQUAL)) {
		return;
	}
	throw NotImplementedException(location,
	                              FractionalComparisonMessage(type == ExpressionType::COMPARE_NOTEQUAL   ? "<>"
	                                                          : type == ExpressionType::COMPARE_LESSTHAN ? "<"
	                                                                                                     : ">",
	                                                          RenderDecideSource(agg, names)));
}

//! Replace the reducer at `site` by its helper decision: the clause's existing one for a
//! repeated occurrence, else a new one, recording what it holds. The reducers inside its
//! argument are replaced first, each by a helper keyed by its path (language/guide.md §5.5), so that what the
//! helper holds is affine in helpers, decisions and data. `outer` is the push of the side the reducer stands in: the
//! comparison's, the objective's (always down), or for a nested one its reader's. `class_key` is the reading clause's
//! (Helpers::ClassKeyOf).
void ReplaceReducer(Helpers &helpers, const DecideSourceNames &names, const BooleanDecisions &booleans,
                    const DecideReducerPath &path, idx_t class_key, ExtremumPush outer, idx_t source_clause_id,
                    const ReducerSite &site) {
	auto &decide = helpers.decide;
	auto &agg = (*site.slot)->Cast<BoundDecideReducerExpression>();
	auto agg_name = StringUtil::Lower(agg.function->name);
	// F6: a diagnosis, and a plan, name the helper by the reducer it holds, `by` included, as written: read before the
	// reducers inside it become helpers.
	auto plan_label = RenderDecideSource(agg, names);
	// The way this reader pushes the helper says which side pins it (DecideHelperDefinition::push). A helper several
	// rows read needs every side any of them does, and the rows read the helper alone: a slack value is harmless.
	const ExtremumPush push = ExtremumPushOf(outer, site.sign);

	// A member is pushed as the helper is, through the sign it carries in the argument: a SUM or AVG is linear in it,
	// and a MIN or MAX does not decrease when it grows, so either passes its push on (D79).
	vector<ReducerSite> nested;
	if (!agg.children.empty()) {
		nested = CollectReducerSites(helpers.context, booleans, agg.children[0]);
	}
	auto below = path.Below(agg);
	for (auto &inner : nested) {
		// The binder refuses a decision times a reducer inside a reducer (D80).
		D_ASSERT(!inner.under_decision);
		ReplaceReducer(helpers, names, booleans, below, class_key, push, source_clause_id, inner);
	}
	// A reducer value link's member program is linear. A quadratic outer SUM whose direct G can emit the Q row stays
	// in the clause (RewriteScopedReducers does that for the compatible path shape); other nested helper definitions
	// retain the established refusal instead of reaching linear term extraction with unsupported Q terms.
	if ((!path.keys.empty() || !nested.empty()) &&
	    !DecideDegreeOf(helpers.context, *agg.children[0], decide.decide_index).IsLinear()) {
		// The reducer as the user wrote it, not as a plan reprints it.
		auto written_names = names;
		written_names.written = true;
		throw BinderException(agg, "'%s' squares or multiplies decisions inside nested reducers, which is not allowed",
		                      RenderDecideSource(agg, written_names));
	}

	// Read once the reducers inside it are helpers, each whole-numbered when its value is.
	const bool whole = DecideValueIsWholeNumbered(agg, decide.decide_index);

	// It reads the rows its path keeps, grouped by its path key: for a reducer at the top, the clause's kept rows by
	// its own `by` key.
	auto rows = path.Rows();
	const idx_t group_key = FindOrCreateDecidePathKey(path.KeysOf(agg.by), decide.bound.keys);
	auto account_kind = group_key == DConstants::INVALID_INDEX ? LogicalDecide::RewriteAccount::Kind::HELPER
	                                                           : LogicalDecide::RewriteAccount::Kind::HELPER_PER_GROUP;
	idx_t link_idx = helpers.Find(agg, rows.get(), group_key, class_key);
	if (link_idx != DConstants::INVALID_INDEX) {
		auto &link = *decide.helper_definitions[link_idx];
		link.push = JoinExtremumPush(link.push, push);
		// Each clause that reads the helper says so in its own plan, once.
		bool told = false;
		for (auto &account : decide.rewrite_accounts) {
			told = told || (account.source_clause_id == source_clause_id && account.kind == account_kind &&
			                account.subject == plan_label);
		}
		if (!told) {
			AddRewriteAccount(decide, source_clause_id, account_kind, plan_label);
		}
		ReplaceWithDecision(helpers.context, decide, link.helper, *site.slot);
		return;
	}

	auto link = make_uniq<DecideHelperDefinition>(DecideHelperKind::REDUCER_VALUE);
	link->agg_name = agg_name;
	// Its columns are named by the reducer it holds, `by` included: a refusal's remedy bounds that (F6).
	link->label = plan_label;
	link->path_key = group_key;
	link->body = agg.Copy();
	link->when = std::move(rows);
	link->class_key = class_key;
	link->push = push;

	// The helper decision: one instance per group of its path key among the rows its path keeps -- `by ()`
	// throughout is one group, or none when no row holds a member.
	KeyedScopeInfo scope;
	scope.key = group_key;
	link_idx = AddValueHelper(decide, std::move(link), scope, whole, source_clause_id, account_kind);
	ReplaceWithDecision(helpers.context, decide, decide.helper_definitions[link_idx]->helper, *site.slot);
}

//! `sum([when c] per K: inner([when c'] e) by (G))` in the objective, `inner` a SUM or AVG with no `per` and no
//! reducer inside, every element of K in G: each G group is one member, so the sum of the members is `inner` read
//! over every group, `c` joined to `c'`. Written so, the objective sums -- or averages per G group -- `e` itself, a
//! square included, which a helper could not define linearly (D81). Replaces the reducer at `slot` and returns true;
//! false when it is not that shape.
bool InlineSumOfGroupSums(LogicalDecide &decide, ClientContext &context, unique_ptr<Expression> &slot) {
	auto &outer = slot->Cast<BoundDecideReducerExpression>();
	const idx_t qualifier = outer.per;
	if (!StringUtil::CIEquals(outer.function->name, "sum") || outer.children.size() != 1 ||
	    outer.by != DConstants::INVALID_INDEX || qualifier >= decide.bound.keyed_scopes.size()) {
		return false;
	}
	auto *body = UnwrapDecideCasts(*outer.children[0], decide.decide_index);
	if (body->GetExpressionClass() != ExpressionClass::BOUND_DECIDE_REDUCER) {
		return false;
	}
	auto &inner = body->Cast<BoundDecideReducerExpression>();
	const bool sum_or_avg =
	    StringUtil::CIEquals(inner.function->name, "sum") || StringUtil::CIEquals(inner.function->name, "avg");
	if (!sum_or_avg || inner.children.size() != 1 || DecideCanonicalizer::ContainsReducer(*inner.children[0]) ||
	    inner.per != DConstants::INVALID_INDEX) {
		return false;
	}
	auto &by = DecideKeyElements(decide.bound.keys, inner.by);
	for (auto &element : DecideKeyElements(decide.bound.keys, decide.bound.keyed_scopes[qualifier].key)) {
		bool in_by = false;
		for (auto &part : by) {
			in_by = in_by || part.Grouping().Equals(element.Grouping());
		}
		if (!in_by) {
			return false; // a G group holding two classes of K would be counted twice
		}
	}
	auto inlined = inner.Copy();
	auto &reducer = inlined->Cast<BoundDecideReducerExpression>();
	reducer.retained = decide.retained_expressions.size();
	decide.retained_expressions.push_back(outer.Copy());
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
		add_condition(MakeDecideNullTest(part.expression->Copy()));
	}
	slot = BoundCastExpression::AddCastToType(context, std::move(inlined), outer.return_type);
	return true;
}

} // namespace

void DecideOptimizer::SwapObjectiveSummation(LogicalDecide &decide) {
	if (!decide.bound.objective.body) {
		return;
	}
	const idx_t decide_index = decide.decide_index;
	DecideTermSplitter splitter(optimizer.context, decide);
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
		auto &outer = slot->Cast<BoundDecideReducerExpression>();
		idx_t frame;
		if (TryGetDecideFrame(outer, frame) || !outer.function || !StringUtil::CIEquals(outer.function->name, "sum") ||
		    outer.children.size() != 1 || outer.per != DConstants::INVALID_INDEX) {
			return;
		}
		auto *body = UnwrapDecideCasts(*outer.children[0], decide_index);
		if (body->GetExpressionClass() != ExpressionClass::BOUND_DECIDE_REDUCER) {
			return;
		}
		auto &inner = body->Cast<BoundDecideReducerExpression>();
		const bool avg = StringUtil::CIEquals(inner.function->name, "avg");
		if ((!avg && !StringUtil::CIEquals(inner.function->name, "sum")) || inner.children.size() != 1 ||
		    TryGetDecideFrame(inner, frame) || inner.per != DConstants::INVALID_INDEX ||
		    DecideCanonicalizer::ContainsReducer(*inner.children[0]) || ContainsDecideFrame(*inner.children[0]) ||
		    (inner.filter && ContainsDecideFrame(*inner.filter)) || splitter.IsLinear(*inner.children[0])) {
			return; // a linear nest is read through helpers, which the model writes in (D89)
		}
		const idx_t group_key = inner.by;
		auto group_count = [&](unique_ptr<Expression> summand) {
			vector<unique_ptr<Expression>> children;
			children.push_back(std::move(summand));
			auto count =
			    make_uniq<BoundDecideReducerExpression>(optimizer.BindAggregateFunction("sum", std::move(children)));
			count->by = group_key;
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
				// A square reaches here in the product form's one spelling (`**`, stage 04 NormalizeProducts), and the
				// weighted square is written in it too, so every reader of a power reads it (IsDecidePowerCall).
				idx_t exponent;
				if (IsDecidePowerCall(func) &&
				    TryGetDecideWholeExponent(optimizer.context, *func.children[1], exponent) && exponent == 2 &&
				    splitter.IsLinear(*func.children[0])) {
					auto root = optimizer.BindScalarFunction("sqrt", w.Copy());
					vector<unique_ptr<Expression>> square;
					square.push_back(
					    optimizer.BindScalarFunction("*", std::move(root), real(func.children[0]->Copy())));
					square.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(2)));
					return DecideRebindOperator(optimizer.context, "**", std::move(square), "power");
				}
			}
			return optimizer.BindScalarFunction("*", w.Copy(), real(term.Copy()));
		};
		vector<unique_ptr<Expression>> children;
		children.push_back(weigh(*inner.children[0]));
		auto swapped =
		    make_uniq<BoundDecideReducerExpression>(optimizer.BindAggregateFunction("sum", std::move(children)));
		auto &reducer = *swapped;
		if (outer.filter) {
			reducer.filter = outer.filter->Copy();
		}
		reducer.SetAlias(outer.GetAlias());
		reducer.CopyFacts(outer);
		reducer.holds_reducer = false; // it holds no reducer over decisions
		slot = BoundCastExpression::AddCastToType(optimizer.context, std::move(swapped), outer.return_type);
	};
	std::function<void(unique_ptr<Expression> &)> visit = [&](unique_ptr<Expression> &expr) {
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER) {
			swap(expr);
			return;
		}
		ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { visit(child); });
	};
	decide.EnumerateBodies([&](DecideOwnedBody &body) {
		if (body.owner == DecideOwnedBody::Owner::OBJECTIVE) {
			visit(body.expr);
		}
	});
}

void DecideOptimizer::RewriteScopedReducers(LogicalDecide &decide) {
	const BooleanDecisions booleans {decide.decide_index, decide.bound.is_boolean_var};
	auto names = DecidePlanNames(decide.bound.source_text, decide.bound.keyed_scopes, decide.bound.keys,
	                             decide.decide_index, decide.aux_var_expressions, decide.bound.frames);
	Helpers helpers {optimizer.context, decide};

	// A Boolean factor on a reducer (`b * sum(x)`, canonicalization admits no other decision) is now a product with
	// the helper. The bilinear rewrite ran before this pass, so it never saw the product: this hands it over, through
	// the one entry (FindAndReplaceBilinear), for the same tree-owned product F node used by ordinary Boolean products.
	auto HandProductsToBilinear = [&](unique_ptr<Expression> &expr, const DecideRowOrigin &origin) {
		FindAndReplaceBilinear(expr, decide, origin);
	};

	// The clauses are rewritten where they sit, the comparisons among them: a placeholder or a bound `IN` is no
	// clause for a reducer to stand in.
	auto rewrite_clause = [&](BoundDecideClause &clause) {
		auto &expr = clause.body;
		auto &scope = clause.scope;
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return;
		}
		// A clause in the VALUE shape holds every decision reducer in a helper. In any other shape SUM and AVG stay in
		// the clause's aggregate row and each MIN or MAX becomes a helper, but for the easy identity, which keeps its
		// reducer (RewriteMinMax).
		const bool value_shape = clause.shape == CanonicalConstraintClass::VALUE;
		if (!value_shape && clause.minmax_easy) {
			return;
		}
		auto &cmp = expr->Cast<BoundComparisonExpression>();
		const idx_t source_clause_id = clause.origin.source_clause_id;
		// Canonicalization put every decision reducer on the left.
		auto sites = CollectReducerSites(optimizer.context, booleans, cmp.left);
		if (!value_shape) {
			sites.erase(
			    std::remove_if(sites.begin(), sites.end(), [](const ReducerSite &site) { return !site.IsExtremum(); }),
			    sites.end());
			if (sites.empty()) {
				return;
			}
		}
		// When a direct outer SUM holds a simple, unfiltered nest in the same clause scope, keep the outer aggregate
		// in the clause. Its G node then reads the nested G values directly, instead of introducing a second helper
		// whose members merely copy the outer SUM. This also lets the ordinary term splitter emit the outer SUM's
		// bilinear or quadratic row. Filtered and deeper nests keep the value-link path until their node-owned
		// formulation can preserve each path's bounds and scope semantics.
		auto *direct_outer = UnwrapDecideCasts(*cmp.left, decide.decide_index);
		vector<ReducerSite> nested_outer_sites;
		if (direct_outer->GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER &&
		    !direct_outer->Cast<BoundDecideReducerExpression>().children.empty()) {
			nested_outer_sites = CollectReducerSites(optimizer.context, booleans,
			                                         direct_outer->Cast<BoundDecideReducerExpression>().children[0]);
		}
		const bool direct_outer_is_sum =
		    direct_outer->GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER &&
		    StringUtil::CIEquals(direct_outer->Cast<BoundDecideReducerExpression>().function->name, "sum");
		const bool nested_outer_has_unfiltered_reducers =
		    !nested_outer_sites.empty() &&
		    std::all_of(nested_outer_sites.begin(), nested_outer_sites.end(), [](const ReducerSite &site) {
			    if (site.slot->get()->GetExpressionClass() != ExpressionClass::BOUND_DECIDE_REDUCER) {
				    return false;
			    }
			    auto &nested = site.slot->get()->Cast<BoundDecideReducerExpression>();
			    return !nested.filter && !nested.children.empty() &&
			           !DecideCanonicalizer::ContainsReducer(*nested.children[0]);
		    });
		// The clause's row is already the outer G's value in this shape. A direct, unfiltered nest with the same
		// clause and outer keys therefore needs no outer value-link column: the inner helper's G is read once per
		// instance by the outer G, and the comparison's push still reaches that inner helper. Keep this deliberately
		// narrow while filtered/deeper cases are handled by the established path-aware links.
		//
		// Kept in the clause, the outer sum makes this the aggregate shape, one row per group, after the bound's
		// rewrite (RewriteRhsAggregates) read the clause in the VALUE shape and left the bound as each row holds it.
		// The two shapes are the same clause only where the bound is one value for the whole query: every row of a
		// clause under no `per` then states the same row, and no class shows a second value. A bound that reads the
		// rows keeps the value-link path, where each row and each variant holds its own.
		const bool bound_is_query_wide = cmp.right->IsFoldable() || clause.query_wide_bound;
		const bool has_nested_outer = direct_outer_is_sum && nested_outer_has_unfiltered_reducers && !scope.when &&
		                              bound_is_query_wide &&
		                              scope.class_key == direct_outer->Cast<BoundDecideReducerExpression>().by;
		DecideReducerPath path;
		if (scope.when) {
			path.conditions.push_back(scope.when.get());
		}
		for (auto &site : sites) {
			if (has_nested_outer && UnwrapDecideCasts(*site.slot->get(), decide.decide_index) == direct_outer) {
				auto &outer = direct_outer->Cast<BoundDecideReducerExpression>();
				auto below = path.Below(outer);
				// A decision times a nested reducer (`sum((x - 1) * max(x) by (g))`) stays under the outer sum, whose
				// term splitter states the product's row.
				for (auto &nested : nested_outer_sites) {
					ReplaceReducer(helpers, names, booleans, below, Helpers::ClassKeyOf(scope),
					               ExtremumPushOf(cmp.type), source_clause_id, nested);
				}
				continue;
			}
			RefuseFractionalStep(site.slot->get()->Cast<BoundDecideReducerExpression>(), names, decide.decide_index,
			                     cmp,
			                     cmp.GetQueryLocation().IsValid() || source_clause_id == DConstants::INVALID_INDEX
			                         ? cmp.GetQueryLocation()
			                         : decide.bound.constraint_sources[source_clause_id].location);
			ReplaceReducer(helpers, names, booleans, path, Helpers::ClassKeyOf(scope), ExtremumPushOf(cmp.type),
			               source_clause_id, site);
		}
		if (value_shape) {
			HandProductsToBilinear(cmp.left, clause.origin);
		}
		// Helpers stand where the reducers were: the clause goes back through the canonicalizing boundary (a helper
		// under a sign is spelled as a decision is), and has the shape of what is left, one row per class or per group
		// of the reducers it keeps. The canonicalizer is made here, to read the helpers' scopes.
		decide.CanonicalizeClause(optimizer.context, clause);
		DecideCanonicalizer(optimizer.context, decide.decide_index, decide.bound.variable_scopes,
		                    decide.bound.keyed_scopes, decide.bound.keys, decide.bound.is_boolean_var,
		                    decide.bound.computed_values)
		    .ClassifyClause(clause);
	};

	// The objective is one expression under its sense (`MAXIMIZE b * sum(x)`): a reducer under a Boolean decision
	// takes a helper decision the same way, and so does a reducer that holds a reducer, the canonicalizer's tag on it
	// (D81), unless it is a sum of group sums read in place; every reducer inside it takes one keyed by its path. So
	// does a reducer a function reads (`0 * sum(x) + abs(sum(x) - 2)`), as in a comparison (a reducer inside another
	// expression is the VALUE shape): the function's value is of one number, which an aggregate row of its envelope
	// would leave per row. A sum of group sums a function reads is no exception: read in place it stays a raw
	// aggregate inside the function, so it takes its helper like any nest. The objective is a minimization (the
	// canonicalizer folded a MAXIMIZE into the term signs), so the push is down times the term's sign, as for the body
	// definitions. A reducer no decision multiplies, no function reads and none nests in stays in the objective as it
	// is.
	auto rewrite_objective = [&](DecideOwnedBody &body) {
		const bool products = body.clause->shape == CanonicalConstraintClass::VALUE;
		auto sites = CollectReducerSites(optimizer.context, booleans, body.expr);
		DecideReducerPath path;
		if (body.rows) {
			path.conditions.push_back(body.rows);
		}
		for (auto &site : sites) {
			const bool nest = (*site.slot)->Cast<BoundDecideReducerExpression>().holds_reducer;
			if (nest && !site.under_decision && !site.under_function &&
			    InlineSumOfGroupSums(decide, optimizer.context, *site.slot)) {
				continue;
			}
			// A MIN or MAX takes a helper in every shape; one a decision multiplies stays for the objective's own
			// refusal unless the product is a Boolean one (`products`).
			if (nest || (site.IsExtremum() && !site.under_decision) || site.under_function ||
			    (products && site.under_decision)) {
				ReplaceReducer(helpers, names, booleans, path, Helpers::ClassKeyOf(body.clause->scope),
				               ExtremumPush::DOWN, DConstants::INVALID_INDEX, site);
			}
		}
		// Every product in the objective, whatever shape it was read in: a reducer a helper replaced inside a function
		// or a nest may stand under a Boolean factor without the shape tag.
		HandProductsToBilinear(body.expr, body.Origin());
	};
	// The objective goes back through the canonicalizing boundary when a helper or a product's link replaced a part
	// of it.
	bool objective_rewritten = false;
	decide.EnumerateBodies([&](DecideOwnedBody &body) {
		if (body.owner == DecideOwnedBody::Owner::CLAUSE) {
			rewrite_clause(*body.clause);
		} else if (body.owner == DecideOwnedBody::Owner::OBJECTIVE) {
			auto written = body.expr->Copy();
			rewrite_objective(body);
			objective_rewritten = !Expression::Equals(*written, *body.expr);
		}
	});
	if (objective_rewritten) {
		decide.SetObjective(optimizer.context, std::move(decide.bound.objective));
	}
}

} // namespace duckdb
