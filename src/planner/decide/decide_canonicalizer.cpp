#include "duckdb/common/decide_profile.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <algorithm>
#include <cmath>

namespace duckdb {

static void CollectExpressionChildren(const Expression &expr, vector<const Expression *> &children) {
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { children.push_back(&child); });
}

//! Expression::Equals deliberately ignores aliases, while DECIDE stores structural
//! and provenance tags in aliases. Canonical fixed-point verification therefore needs
//! the normal semantic comparison plus an ordered, recursive alias comparison.
static bool CanonicalTreesEqual(const Expression &left, const Expression &right) {
	if (!Expression::Equals(left, right) || left.GetAlias() != right.GetAlias()) {
		return false;
	}
	vector<const Expression *> left_children;
	vector<const Expression *> right_children;
	CollectExpressionChildren(left, left_children);
	CollectExpressionChildren(right, right_children);
	if (left_children.size() != right_children.size()) {
		return false;
	}
	for (idx_t i = 0; i < left_children.size(); i++) {
		if (!CanonicalTreesEqual(*left_children[i], *right_children[i])) {
			return false;
		}
	}
	return true;
}

bool TryMatchScaledAggregate(const Expression &expr, idx_t decide_index, ScaledAggregateMatch &result) {
	result = {};
	auto current = UnwrapDecideCasts(expr, decide_index);
	if (current->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func = current->Cast<BoundFunctionExpression>();
	bool is_mult = func.function.name == "*";
	bool is_div = func.function.name == "/";
	if ((!is_mult && !is_div) || func.children.size() != 2) {
		return false;
	}

	idx_t aggregate_child;
	if (is_div && UnwrapDecideCasts(*func.children[0], decide_index)->GetExpressionClass() ==
	                  ExpressionClass::BOUND_AGGREGATE) {
		aggregate_child = 0;
	} else if (is_mult && UnwrapDecideCasts(*func.children[1], decide_index)->GetExpressionClass() ==
	                         ExpressionClass::BOUND_AGGREGATE) {
		aggregate_child = 1;
	} else {
		return false;
	}
	if (UnwrapDecideCasts(*func.children[1 - aggregate_child], decide_index)->GetExpressionClass() ==
	    ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}

	result.aggregate = &UnwrapDecideCasts(*func.children[aggregate_child], decide_index)
	                        ->Cast<BoundAggregateExpression>();
	result.scale = func.children[1 - aggregate_child].get();
	result.function = &func;
	result.divides = is_div;
	return true;
}

//! The additive spine: `+`, binary and unary `-`, and binder casts over decision algebra. Every other node is a
//! term, handed to `callback` with its accumulated sign. Decompose and ReadCanonicalAtoms both walk it, so the
//! terms a reader sees are the ones canonicalization placed.
template <class IS_DECISION, class CALLBACK>
static void WalkAdditiveSpine(const Expression &expr, int sign, IS_DECISION &&is_decision, CALLBACK &&callback) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.function.name == "+" && !func.children.empty()) {
			for (auto &child : func.children) {
				WalkAdditiveSpine(*child, sign, is_decision, callback);
			}
			return;
		}
		if (func.function.name == "-" && func.children.size() == 2) {
			WalkAdditiveSpine(*func.children[0], sign, is_decision, callback);
			WalkAdditiveSpine(*func.children[1], -sign, is_decision, callback);
			return;
		}
		if (func.function.name == "-" && func.children.size() == 1) {
			WalkAdditiveSpine(*func.children[0], -sign, is_decision, callback);
			return;
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST && is_decision(expr)) {
		// The parsed boundary rejected every user-authored decision cast. Anything left here was inserted by
		// binding and is transparent in the solver's single DOUBLE domain. Data-only casts stay atomic.
		WalkAdditiveSpine(*expr.Cast<BoundCastExpression>().child, sign, is_decision, callback);
		return;
	}
	callback(sign, expr);
}

ExtremumBody SplitExtremumBody(const Expression &body, idx_t decide_index) {
	// Adding one number to every row does not change which row is largest; a column addend varies by row and stays.
	auto is_decision = [&](const Expression &node) {
		return BoundExpressionReferencesDecide(node, decide_index);
	};
	ExtremumBody split;
	WalkAdditiveSpine(body, 1, is_decision, [&](int sign, const Expression &term) {
		(term.IsFoldable() && !is_decision(term) ? split.moved : split.kept).emplace_back(sign, &term);
	});
	return split;
}

vector<CanonicalAtom> ReadCanonicalAtoms(const Expression &side, idx_t decide_index) {
	vector<CanonicalAtom> atoms;
	WalkAdditiveSpine(
	    side, 1, [&](const Expression &expr) { return BoundExpressionReferencesDecide(expr, decide_index); },
	    [&](int sign, const Expression &term) {
		    CanonicalAtom atom;
		    atom.sign = sign;
		    atom.term = &term;
		    atom.scaled = TryMatchScaledAggregate(term, decide_index, atom.scale);
		    atoms.push_back(atom);
	    });
	return atoms;
}

[[noreturn]] static void CanonicalInvariantFailure(const string &rule, const string &detail,
                                                   const Expression &expr) {
	throw InternalException("DECIDE canonical invariant %s violated: %s. Expression: %s", rule, detail,
	                        expr.ToString());
}

DecideCanonicalizer::DecideCanonicalizer(ClientContext &context, idx_t decide_index,
                                         vector<DecideVarScopeInfo> variable_scopes, const vector<bool> &is_boolean)
    : context(context), decide_index(decide_index), variable_scopes(std::move(variable_scopes)),
      boolean_decisions(is_boolean), judge_column_refs(false) {
}

DecideCanonicalizer::DecideCanonicalizer(ClientContext &context, idx_t decide_index,
                                         vector<DecideVarScopeInfo> variable_scopes, const vector<bool> &is_boolean,
                                         unordered_set<idx_t> query_wide_table_indexes,
                                         unordered_set<idx_t> correlated_subquery_table_indexes)
    : context(context), decide_index(decide_index), variable_scopes(std::move(variable_scopes)),
      boolean_decisions(is_boolean), query_wide_table_indexes(std::move(query_wide_table_indexes)),
      correlated_subquery_table_indexes(std::move(correlated_subquery_table_indexes)),
      judge_column_refs(true) {
}

bool DecideCanonicalizer::ReferencesDecideVar(const Expression &expr) const {
	return BoundExpressionReferencesDecide(expr, decide_index);
}

//! True when `expr` is NULL exactly when one of its columns is: a column, a constant, or
//! `+`, `-`, `*` and casts over such. A division can be NULL over values (by zero).
static bool NullOnlyFromColumns(const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
	case ExpressionClass::BOUND_CONSTANT:
		return true;
	case ExpressionClass::BOUND_CAST:
		if (expr.Cast<BoundCastExpression>().try_cast) {
			return false;
		}
		break;
	case ExpressionClass::BOUND_FUNCTION: {
		auto &name = expr.Cast<BoundFunctionExpression>().function.name;
		if (name != "+" && name != "-" && name != "*") {
			return false;
		}
		break;
	}
	default:
		return false;
	}
	bool only = true;
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](const Expression &child) { only = only && NullOnlyFromColumns(child); });
	return only;
}

static void AddNullTest(const Expression &value, vector<unique_ptr<Expression>> &out) {
	for (auto &seen : out) {
		if (seen->Equals(value)) {
			return;
		}
	}
	out.push_back(value.Copy());
}

static void AddColumnsOf(const Expression &expr, vector<unique_ptr<Expression>> &out) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		AddNullTest(expr, out);
		return;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { AddColumnsOf(child, out); });
}

//! The values to test so that `expr` is not NULL where a NULL makes it NULL: of each largest
//! decision-free part that is not a constant, its columns when only they can make it NULL,
//! else the part itself; looking through casts and functions with SQL's default NULL
//! handling only. A nested reducer is its own reducer and is not looked into.
static void CollectNullableData(const Expression &expr, idx_t decide_index, vector<unique_ptr<Expression>> &out) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		return;
	}
	if (!BoundExpressionReferencesDecide(expr, decide_index)) {
		if (expr.IsFoldable()) {
			return;
		}
		if (NullOnlyFromColumns(expr)) {
			AddColumnsOf(expr, out);
		} else {
			AddNullTest(expr, out);
		}
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		if (expr.Cast<BoundFunctionExpression>().function.null_handling !=
		    FunctionNullHandling::DEFAULT_NULL_HANDLING) {
			return;
		}
	} else if (expr.GetExpressionClass() != ExpressionClass::BOUND_CAST) {
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { CollectNullableData(child, decide_index, out); });
}

//! The written key columns of each keyed decision `expr` reads outside a nested reducer: a
//! NULL there leaves the row with no decision (a relation's columns compare NULL as a value).
static void CollectAbsentDecisionKeys(const Expression &expr, idx_t decide_index,
                                      const vector<DecideVarScopeInfo> &variable_scopes,
                                      const vector<EntityScopeInfo> &scopes, const vector<DecideKey> &keys,
                                      vector<unique_ptr<Expression>> &out) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &binding = expr.Cast<BoundColumnRefExpression>().binding;
		auto var = binding.column_index;
		if (binding.table_index != decide_index || var >= variable_scopes.size() ||
		    variable_scopes[var].scope != DecideVarScope::ENTITY) {
			return;
		}
		for (auto &element : DecideKeyElements(keys, scopes[variable_scopes[var].entity_scope_idx].key)) {
			if (element.null_mode == DecideKeyNullMode::NO_GROUP) {
				AddNullTest(*element.expression, out);
			}
		}
		return;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		CollectAbsentDecisionKeys(child, decide_index, variable_scopes, scopes, keys, out);
	});
}

void DecideCanonicalizer::SkipNullData(Expression &expr, const vector<EntityScopeInfo> &scopes,
                                       const vector<DecideKey> &keys) const {
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { SkipNullData(child, scopes, keys); });
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE || !ReferencesDecideVar(expr)) {
		return;
	}
	auto &reducer = expr.Cast<BoundAggregateExpression>();
	vector<unique_ptr<Expression>> data;
	for (auto &child : reducer.children) {
		CollectNullableData(*child, decide_index, data);
		CollectAbsentDecisionKeys(*child, decide_index, variable_scopes, scopes, keys, data);
	}
	for (auto &value : data) {
		auto test = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
		test->children.push_back(std::move(value));
		test->SetAlias(SKIP_NULL_TAG);
		if (!reducer.filter) {
			reducer.filter = std::move(test);
			continue;
		}
		reducer.filter = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
		                                                       std::move(reducer.filter), std::move(test));
	}
}

bool DecideCanonicalizer::ContainsReducer(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = ContainsReducer(child);
		}
	});
	return found;
}

DecideCanonicalizer::Placement DecideCanonicalizer::Classify(const Expression &expr) const {
	if (ReferencesDecideVar(expr)) {
		return Placement::LEFT;
	}
	// Everything decision-free is a bound, including a data-only reducer. The left
	// side has no aggregate evaluator -- SumFixedAggregateLhsOffset merely SUMS a term's
	// column over the group, which is why only SUM and AVG could ever be hoisted
	// there and MIN/MAX/COUNT were refused. A reducer on the right is any DuckDB
	// aggregate over its groups (RewriteRhsAggregates), so that is where it belongs.
	return Placement::RIGHT;
}


bool DecideCanonicalizer::IsBooleanFactor(const Expression &expr) const {
	auto *cur = UnwrapDecideCasts(expr, decide_index);
	if (cur->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		// A decision column: a factor reaches here only for bearing one.
		auto column = cur->Cast<BoundColumnRefExpression>().binding.column_index;
		return column < boolean_decisions.size() && boolean_decisions[column];
	}
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func = cur->Cast<BoundFunctionExpression>();
	if (func.function.name == "-" && func.children.size() == 1) {
		return IsBooleanFactor(*func.children[0]); // the sign FoldNegationIntoFactor gives a negated reducer
	}
	if (func.function.name == "*" && func.children.size() == 2) {
		// Data times the Boolean: the binder admits no product of two decisions beside a reducer (degree 3), so
		// exactly one side bears the decision.
		return IsBooleanFactor(*func.children[ReferencesDecideVar(*func.children[0]) ? 0 : 1]);
	}
	return false;
}

bool DecideCanonicalizer::IsQueryWideConstant(const Expression &expr) const {
	// A factor on a reducer has to answer "are you one value for the whole query?", and
	// for a column ref that cannot be answered from shape: flattening leaves
	// `(SELECT max(w) FROM p)` looking exactly like `weight`, down to being a
	// BoundColumnRefExpression on a table index of its own.
	//
	// So the answer comes from evidence collected before flattening, and the test is
	// an ALLOW-list: query-wide iff positively marked. Framing it the other way --
	// "not one of the reduced relation's own bindings" -- reads identically for
	// `weight` but silently admits a CORRELATED subquery, which also lands on a fresh
	// table index yet yields a different value per row.
	if (IsQueryWideValueTag(expr.GetAlias())) {
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		if (!judge_column_refs) {
			return false; // No evidence available: never promote an arbitrary column.
		}
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		return query_wide_table_indexes.find(colref.binding.table_index) !=
		       query_wide_table_indexes.end();
	}
	// An unflattened uncorrelated scalar subquery is one value by definition. A
	// correlated one is genuinely per-row and must not qualify.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		auto &subq = expr.Cast<BoundSubqueryExpression>();
		return subq.subquery_type == SubqueryType::SCALAR && !subq.IsCorrelated();
	}
	if (expr.IsFoldable()) {
		return true;
	}
	// A factor that is identically zero before the query runs is one value for the
	// whole query -- 0 -- whatever its data factors hold: `0 * w` does not vary by row.
	if (PlanTimeSignOf(context, expr) == PlanTimeSign::ZERO) {
		return true;
	}
	// Arithmetic over query-wide values is itself query-wide, which is what makes
	// `2 * (SELECT k FROM p) * SUM(x)` work: neither factor folds on its own.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.children.empty()) {
			return false;
		}
		for (auto &child : func.children) {
			if (!IsQueryWideConstant(*child)) {
				return false;
			}
		}
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		return IsQueryWideConstant(*expr.Cast<BoundCastExpression>().child);
	}
	return false;
}

bool DecideCanonicalizer::IsQueryWideExpression(const Expression &expr) const {
	if (IsQueryWideValueTag(expr.GetAlias())) {
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (colref.binding.table_index == decide_index) {
			auto var_idx = colref.binding.column_index;
			return var_idx < variable_scopes.size() && variable_scopes[var_idx].IsScalar();
		}
		return judge_column_refs &&
		       query_wide_table_indexes.find(colref.binding.table_index) != query_wide_table_indexes.end();
	}
	// Reducers are group-wide rather than query-wide: their legality is decided as
	// reducer terms by ClassifyCanonicalComparison, not recursively here.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		auto &subq = expr.Cast<BoundSubqueryExpression>();
		return subq.subquery_type == SubqueryType::SCALAR && !subq.IsCorrelated();
	}
	if (expr.IsFoldable()) {
		return true;
	}
	bool saw_child = false;
	bool all_query_wide = true;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		saw_child = true;
		if (all_query_wide && !IsQueryWideExpression(child)) {
			all_query_wide = false;
		}
	});
	return saw_child && all_query_wide;
}

unique_ptr<Expression>
DecideCanonicalizer::FinalizeBoundProvenance(unique_ptr<BoundComparisonExpression> comparison) const {
	// Root classification describes the complete canonical bound, so never trust a
	// copied value blindly: optimizer rewrites may have replaced children since the
	// previous canonicalization. Component-level QUERY_WIDE_VALUE_TAG facts remain and
	// are the semantic leaves from which the classification is recomputed.
	auto alias = comparison->right->GetAlias();
	RemoveDecideTag(alias, QUERY_WIDE_BOUND_TAG);
	comparison->right->SetAlias(std::move(alias));
	if (IsQueryWideConstant(*comparison->right)) {
		auto bound_alias = comparison->right->GetAlias();
		AddDecideTag(bound_alias, QUERY_WIDE_BOUND_TAG);
		comparison->right->SetAlias(std::move(bound_alias));
	}
	return std::move(comparison);
}

//! The norm `aggregate` was written as, with its body, or nullptr when it was not one: a desugared norm carries
//! its order in a display tag, and an L0 norm is still the binder's marker.
static const Expression *WrittenNormBody(const BoundAggregateExpression &aggregate, string &order) {
	if (aggregate.children.size() != 1) {
		return nullptr;
	}
	const Expression *body;
	if (ExtractDecideTagPayload(aggregate.GetAlias(), NORM_MARKER_TAG_PREFIX, order)) {
		body = aggregate.children[0].get();
	} else if (ExtractDecideTagPayload(aggregate.GetAlias(), WRITTEN_NORM_TAG_PREFIX, order)) {
		body = DecideWrittenNormBody(aggregate);
	} else {
		return nullptr;
	}
	order = order == "0_auto" ? "0" : order.rfind("0_", 0) == 0 ? "0, " + order.substr(2) : order;
	return body;
}

//! Name an expression the way the user wrote it, for an error message. Everything the
//! binder added is noise here: `CAST(weight AS DECIMAL(12,1))` is not what anyone
//! typed, and neither is the `FILTER (WHERE w)` that a reducer's own `when` becomes.
static string UserFacingName(const Expression &expr) {
	const Expression *cur = StripCastsForIdentity(expr);
	if (cur->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &agg = cur->Cast<BoundAggregateExpression>();
		string order;
		if (auto body = WrittenNormBody(agg, order)) {
			return "NORM(" + UserFacingName(*body) + ", " + order + ")";
		}
		if (agg.children.size() == 1) {
			return StringUtil::Upper(agg.function.name) + "(" + UserFacingName(*agg.children[0]) + ")";
		}
		return StringUtil::Upper(agg.function.name) + "(...)";
	}
	auto name = StripDecideTags(cur->GetName());
	return name.empty() ? cur->ToString() : name;
}

bool DecideCanonicalizer::IsCorrelatedSubqueryRef(const Expression &expr) const {
	const Expression *cur = StripCastsForIdentity(expr);
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto idx = cur->Cast<BoundColumnRefExpression>().binding.table_index;
	return correlated_subquery_table_indexes.find(idx) != correlated_subquery_table_indexes.end();
}

const Expression &DecideCanonicalizer::PeelScale(const Expression &expr, unique_ptr<Expression> &out_scale,
                                                 bool &out_divides, Clause clause) const {
	out_scale = nullptr;
	out_divides = false;
	const char *clause_name = clause == Clause::OBJECTIVE ? "DECIDE objective" : "DECIDE constraint";

	// Only a DECISION-BEARING reducer is scaled here: a data-only reducer
	// (`2 * AVG(price)` as a bound) is the RHS evaluator's business, and reaching into
	// it would reject shapes this pass has no stake in.
	auto is_scalable = [&](const Expression &e) {
		return ContainsReducer(e) && ReferencesDecideVar(e);
	};

	// Factors are gathered by role, not in encounter order, so that one composition
	// rule covers `M * AGG`, `AGG / D` and every nesting of the two.
	vector<const Expression *> multipliers;
	vector<const Expression *> divisors;
	const Expression *cur = &expr;

	while (true) {
		// Casts around decision-bearing algebra are binder-inserted type noise. A
		// data-only cast cannot reach this walk as `cur`, because `is_scalable`
		// requires the subtree to contain a decision-bearing reducer.
		cur = UnwrapDecideCasts(*cur, decide_index);
		if (cur->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
			break;
		}
		auto &func = cur->Cast<BoundFunctionExpression>();
		bool is_mult = func.function.name == "*";
		bool is_div = func.function.name == "/";
		if ((!is_mult && !is_div) || func.children.size() != 2) {
			break;
		}

		const Expression *term = nullptr;
		const Expression *factor = nullptr;
		bool factor_divides = false;
		if (is_mult && is_scalable(*func.children[0]) && !ContainsReducer(*func.children[1])) {
			term = func.children[0].get();
			factor = func.children[1].get();
		} else if (is_mult && is_scalable(*func.children[1]) && !ContainsReducer(*func.children[0])) {
			term = func.children[1].get();
			factor = func.children[0].get();
		} else if (is_div && is_scalable(*func.children[0]) && !ContainsReducer(*func.children[1])) {
			// Only `AGG / K` divides. `K / AGG` is not a scaled reducer at all.
			term = func.children[0].get();
			factor = func.children[1].get();
			factor_divides = true;
		} else {
			// Two reducers multiplied, or no reducer at all (`price * x` is an ordinary
			// per-row coefficient). Not a scale; stop here.
			break;
		}

		// The single judgement. A reducer collapses many rows to one number, so anything
		// multiplying it must also be one number. Each nesting level is judged
		// separately, so the message names the factor the user actually has to fix.
		const char *verb = factor_divides ? "divide" : "multiply";
		if (ReferencesDecideVar(*factor)) {
			// A constraint takes any data factor (a row-varying one is a tuple term, below), and a clause of
			// either kind a Boolean decision on a bare reducer: the clause takes the VALUE shape, where the
			// reducer becomes a helper decision and the product `b * helper` is linearized as every Boolean
			// product is. An INT or REAL decision is not convex. (A divisor that bears a decision is refused
			// by the binder, before this runs.)
			const bool boolean_factor = IsBooleanFactor(*factor) &&
			                            UnwrapDecideCasts(*term, decide_index)->GetExpressionClass() ==
			                                ExpressionClass::BOUND_AGGREGATE;
			if (!boolean_factor) {
				throw BinderException("%s: '%s' is a decision, so it cannot %s %s. %s", clause_name,
				                      UserFacingName(*factor), verb, UserFacingName(*term),
				                      clause == Clause::CONSTRAINT
				                          ? "Only data or a BOOL decision can scale SUM/AVG/MIN/MAX."
				                          : "Only constants, query-wide values or a BOOL decision can scale "
				                            "SUM/AVG/MIN/MAX.");
			}
			multipliers.push_back(factor);
			cur = term;
			continue;
		}
		// A row-varying factor on a reducer of a constraint is a tuple term of the row's
		// constraint: the comparison takes the VALUE shape (ClassifyScoped), which
		// multiplies the reducer's helper decision by the factor's value on each row. The
		// objective, produced once, refuses it.
		bool row_varying_tuple_term = clause == Clause::CONSTRAINT;
		if (!IsQueryWideConstant(*factor) && !row_varying_tuple_term) {
			// A correlated subquery has no SQL identifier to quote -- flattening left it a
			// column ref named "SUBQUERY" -- so name it for what the user wrote instead of
			// echoing an internal name and suggesting `SUM(x * SUBQUERY)`, which is not
			// something anyone can type.
			if (IsCorrelatedSubqueryRef(*factor)) {
				throw BinderException(
				    "%s: this subquery returns a different value for each row, "
				    "so it cannot %s %s. Move it inside the aggregate, e.g. "
				    "SUM(x %s (SELECT ...)).",
				    clause_name, verb, UserFacingName(*term), factor_divides ? "/" : "*");
			}
			throw BinderException(
			    "%s: '%s' varies per row, so it cannot %s %s. "
			    "Move it inside the aggregate, e.g. SUM(x %s %s).",
			    clause_name, UserFacingName(*factor), verb, UserFacingName(*term),
			    factor_divides ? "/" : "*", UserFacingName(*factor));
		}

		(factor_divides ? divisors : multipliers).push_back(factor);
		cur = term;
	}

	if (multipliers.empty() && divisors.empty()) {
		return expr;
	}

	// Compose each role into one node, then combine the two. `M`, `D` and `M / D` are
	// the only three results, so the rebuild in BuildAdditive stays a single level and
	// the physical extractor's single-level match keeps working unchanged.
	auto product = [&](const vector<const Expression *> &factors) {
		unique_ptr<Expression> acc;
		for (auto *factor : factors) {
			acc = acc ? BindOp("*", std::move(acc), factor->Copy()) : factor->Copy();
		}
		return acc;
	};
	auto multiplier = product(multipliers);
	auto divisor = product(divisors);

	if (!divisor) {
		out_scale = std::move(multiplier);
	} else if (!multiplier) {
		out_scale = std::move(divisor);
		out_divides = true;
	} else {
		// Mixed nesting collapses to a single multiplication by `M / D` rather than
		// keeping a numerator and a denominator slot: one spelling downstream beats two.
		out_scale = BindOp("/", std::move(multiplier), std::move(divisor));
	}
	return *cur;
}

//! An additive spine over decisions: `+`, binary or unary `-` under the binder's casts.
static bool IsAdditiveSpine(const Expression &expr, idx_t decide_index) {
	auto *cur = UnwrapDecideCasts(expr, decide_index);
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func = cur->Cast<BoundFunctionExpression>();
	return (func.function.name == "+" && func.children.size() == 2) ||
	       (func.function.name == "-" && (func.children.size() == 1 || func.children.size() == 2));
}

unique_ptr<Expression> DecideCanonicalizer::ComposeScale(const Expression *outer, bool outer_divides,
                                                         unique_ptr<Expression> inner, bool inner_divides,
                                                         bool &out_divides) const {
	if (!outer) {
		out_divides = inner_divides;
		return inner;
	}
	if (!inner) {
		out_divides = outer_divides;
		return outer->Copy();
	}
	// The same three results PeelScale composes to: `M`, `D` or `M / D`.
	if (!outer_divides && !inner_divides) {
		out_divides = false;
		return BindOp("*", outer->Copy(), std::move(inner)); // c * (d * e) = (c * d) * e
	}
	if (outer_divides && inner_divides) {
		out_divides = true;
		return BindOp("*", std::move(inner), outer->Copy()); // (e / d) / c = e / (d * c)
	}
	out_divides = false;
	return outer_divides ? BindOp("/", std::move(inner), outer->Copy())  // (d * e) / c = (d / c) * e
	                     : BindOp("/", outer->Copy(), std::move(inner)); // c * (e / d) = (c / d) * e
}

void DecideCanonicalizer::Decompose(const Expression &expr, int sign, vector<Atom> &out, Clause clause,
                                    const Expression *factor, bool factor_divides) const {
	WalkAdditiveSpine(
	    expr, sign, [&](const Expression &node) { return ReferencesDecideVar(node); },
	    [&](int term_sign, const Expression &whole) {
		    // A factor known before the query runs distributes over an additive spine, as a
		    // negation does: `2 * (min(x) - 1)` is `2 * min(x) - 2 * 1`, the one spelling
		    // a scaled reducer beside a constant has, so the MIN/MAX rewrite and singleton-row
		    // folding read it as they read `2 * min(x) - 2`. A factor that varies by row,
		    // or a subquery, stays on the spine as one atom: its value is the VALUE shape's.
		    if (whole.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			    auto &func = whole.Cast<BoundFunctionExpression>();
			    if ((func.function.name == "*" || func.function.name == "/") && func.children.size() == 2) {
				    const bool divides = func.function.name == "/";
				    for (idx_t side = divides ? 1 : 0; side < 2; side++) {
					    auto &candidate = *func.children[side];
					    auto &spine = *func.children[1 - side];
					    if (candidate.IsFoldable() && !ReferencesDecideVar(candidate) &&
					        IsAdditiveSpine(spine, decide_index) && ReferencesDecideVar(spine)) {
						    bool composed_divides;
						    auto composed =
						        ComposeScale(factor, factor_divides, candidate.Copy(), divides, composed_divides);
						    Decompose(spine, term_sign, out, clause, composed.get(), composed_divides);
						    return;
					    }
				    }
			    }
		    }
		    // Every other node is a term boundary -- but a factor sitting on a reducer is
		    // peeled off it first, so the term the rest of the pipeline sees is the bare
		    // reducer and the factor travels beside it; a factor distributed from above
		    // composes with it.
		    unique_ptr<Expression> scale;
		    bool divides = false;
		    auto &term = PeelScale(whole, scale, divides, clause);
		    if (factor) {
			    scale = ComposeScale(factor, factor_divides, std::move(scale), divides, divides);
		    }
		    if (DecomposeExtremumAddends(term_sign, term, whole, scale, divides, out)) {
			    return;
		    }
		    // Classify on the whole original term. The factors are decision-free and
		    // reducer-free by construction, so this agrees with classifying `term` -- stated
		    // rather than assumed, because Classify is the part that was wrong twice before.
		    out.push_back(Atom {term_sign, &term, Classify(whole), std::move(scale), divides});
	    });
}

bool DecideCanonicalizer::DecomposeExtremumAddends(int sign, const Expression &term, const Expression &whole,
                                                   unique_ptr<Expression> &scale, bool divides,
                                                   vector<Atom> &out) const {
	// `max(x + 0.5)` is `max(x) + 0.5`: each constant addend joins the side under the reducer's factor, so it reaches
	// the bound and a strict bound steps on `max(x)`.
	auto &reducer = *UnwrapDecideCasts(term, decide_index);
	if (reducer.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	auto &aggregate = reducer.Cast<BoundAggregateExpression>();
	const auto &name = aggregate.function.name;
	if ((name != "min" && name != "max") || aggregate.children.size() != 1) {
		return false;
	}
	auto split = SplitExtremumBody(*aggregate.children[0], decide_index);
	if (split.moved.empty()) {
		return false;
	}
	vector<Atom> kept;
	for (auto &part : split.kept) {
		kept.push_back(Atom {part.first, part.second, Classify(*part.second)});
	}
	auto owned = RebindReducer(aggregate, name, BuildAdditive(kept, false));
	auto *rebuilt = owned.get();
	auto *factor = scale.get();
	out.push_back(Atom {sign, rebuilt, Classify(whole), std::move(scale), divides, std::move(owned)});
	for (auto &part : split.moved) {
		out.push_back(
		    Atom {sign * part.first, part.second, Classify(*part.second), factor ? factor->Copy() : nullptr, divides});
	}
	return true;
}

//! Whether `expr` is a norm marker whose order has an expression for a definition: 1, 2, 'inf', and an L0 norm of
//! data. An L0 norm of a decision does not: its count needs indicator variables, which only the optimizer can add
//! (DecideOptimizer::RewriteNorm).
static bool IsDesugarableNorm(const Expression &expr, idx_t decide_index, string &order) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE ||
	    !ExtractDecideTagPayload(expr.GetAlias(), NORM_MARKER_TAG_PREFIX, order)) {
		return false;
	}
	return order == "1" || order == "2" || order == "inf" || !BoundExpressionReferencesDecide(expr, decide_index);
}

static bool ContainsDesugarableNorm(const Expression &expr, idx_t decide_index) {
	string order;
	if (IsDesugarableNorm(expr, decide_index, order)) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { found = found || ContainsDesugarableNorm(child, decide_index); });
	return found;
}

unique_ptr<Expression> DecideCanonicalizer::DesugarNorms(const Expression &expr) const {
	if (!ContainsDesugarableNorm(expr, decide_index)) {
		return nullptr;
	}
	auto result = expr.Copy();
	DesugarNormsInPlace(result);
	return result;
}

void DecideCanonicalizer::DesugarNormsInPlace(unique_ptr<Expression> &expr) const {
	string order;
	if (!IsDesugarableNorm(*expr, decide_index, order)) {
		// A marker stands wherever a scalar may: under a comparison, `+`, `*` or a cast.
		ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { DesugarNormsInPlace(child); });
		return;
	}
	auto &marker = expr->Cast<BoundAggregateExpression>();
	if (marker.children.size() != 1) {
		throw InternalException("DECIDE norm marker must contain one bound expression");
	}
	auto &body = *marker.children[0];
	unique_ptr<Expression> definition;
	if (order == "2") {
		definition = RebindReducer(marker, "sum",
		                           BindOp("power", body.Copy(), make_uniq<BoundConstantExpression>(Value::INTEGER(2))));
	} else if (order == "1" || order == "inf") {
		definition = RebindReducer(marker, order == "1" ? "sum" : "max", BindOp("abs", body.Copy()));
	} else {
		// An L0 norm of data counts the rows whose value is not 0; its `M` bounds a decision and means nothing here.
		// SIGN reads a NaN as 0, so a value check reads the body (DecideWrittenNormBody), not this definition.
		definition = RebindReducer(marker, "sum", BindOp("abs", BindOp("sign", body.Copy())));
	}
	auto alias = definition->GetAlias();
	RemoveDecideTag(alias, string(NORM_MARKER_TAG_PREFIX) + order + "__");
	AddDecideTag(alias, string(WRITTEN_NORM_TAG_PREFIX) + order + "__");
	definition->SetAlias(std::move(alias));
	expr = std::move(definition);
}

unique_ptr<Expression> DecideCanonicalizer::BindAggregate(const string &name, unique_ptr<Expression> child) const {
	auto &entry = Catalog::GetSystemCatalog(context)
	                  .GetEntry(context, CatalogType::SCALAR_FUNCTION_ENTRY, DEFAULT_SCHEMA, name)
	                  .Cast<AggregateFunctionCatalogEntry>();
	FunctionBinder function_binder(context);
	ErrorData error;
	vector<LogicalType> argument_types {child->return_type};
	auto best = function_binder.BindFunction(entry.name, entry.functions, argument_types, error);
	if (!best.IsValid()) {
		throw InternalException("DECIDE canonicalizer failed to bind aggregate '%s': %s", name, error.Message());
	}
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(child));
	return function_binder.BindAggregateFunction(entry.functions.GetFunctionByOffset(best.GetIndex()),
	                                             std::move(children));
}

unique_ptr<Expression> DecideCanonicalizer::RebindReducer(const BoundAggregateExpression &from, const string &name,
                                                          unique_ptr<Expression> child) const {
	auto result = BindAggregate(name, std::move(child));
	result->SetAlias(from.GetAlias());
	if (from.filter) {
		result->Cast<BoundAggregateExpression>().filter = from.filter->Copy();
	}
	return result;
}

//! The bare decision `expr` is a constant multiple of, with that multiple folded into
//! `value` -- `2 * x`, `x * 2`, `-x`, `x / 2` -- or `expr` itself with `value` untouched
//! when it is not one. PeelScale peels factors off reducers only, so a plain decision
//! carries its factor inside the atom; the like-term merge needs it outside.
static const Expression *PeelBareDecisionFactors(ClientContext &context, const Expression &expr, idx_t decide_index,
                                                 double &value) {
	auto *cur = UnwrapDecideCasts(expr, decide_index);
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return cur;
	}
	auto &func = cur->Cast<BoundFunctionExpression>();
	const auto &name = func.function.name;
	auto constant = [&](const Expression &candidate, double &c) {
		return TryEvaluateFoldableDouble(context, candidate, c, FOLD_CATCH_ALL) && std::isfinite(c) && c != 0.0;
	};
	double c;
	double inner = 1.0;
	const Expression *term = nullptr;
	if (name == "-" && func.children.size() == 1) {
		term = PeelBareDecisionFactors(context, *func.children[0], decide_index, inner);
		inner = -inner;
	} else if (name == "*" && func.children.size() == 2) {
		for (idx_t side = 0; side < 2 && !term; side++) {
			if (constant(*func.children[side], c)) {
				term = PeelBareDecisionFactors(context, *func.children[1 - side], decide_index, inner);
				inner *= c;
			}
		}
	} else if (name == "/" && func.children.size() == 2 && constant(*func.children[1], c)) {
		term = PeelBareDecisionFactors(context, *func.children[0], decide_index, inner);
		inner /= c;
	}
	if (!term || term->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return cur;
	}
	value *= inner;
	return term;
}

bool DecideCanonicalizer::MergeLikeAtoms(vector<Atom> &atoms) const {
	// The term an atom is a multiple of, and that multiple as a number known before the
	// query runs: the factor PeelScale peeled off a reducer (none is 1, a divisor its
	// reciprocal), with the constant factors a bare decision stands under folded in.
	// False when the factor is data or a subquery, which is not summable here.
	auto key_of = [&](const Atom &atom, const Expression *&term, double &value) {
		value = 1.0;
		if (atom.scale) {
			double scale;
			if (!TryEvaluateFoldableDouble(context, *atom.scale, scale, FOLD_PLAIN) || !std::isfinite(scale) ||
			    (atom.scale_divides && scale == 0.0)) {
				return false;
			}
			value = atom.scale_divides ? 1.0 / scale : scale;
			term = atom.expr;
			return true;
		}
		term = PeelBareDecisionFactors(context, *atom.expr, decide_index, value);
		return true;
	};
	bool merged = false;
	for (idx_t i = 0; i < atoms.size(); i++) {
		const Expression *term;
		double total;
		if (atoms[i].placement != Placement::LEFT || !key_of(atoms[i], term, total)) {
			continue;
		}
		total *= atoms[i].sign;
		vector<idx_t> like;
		for (idx_t j = i + 1; j < atoms.size(); j++) {
			const Expression *other;
			double value;
			if (atoms[j].placement == Placement::LEFT && key_of(atoms[j], other, value) &&
			    CanonicalTreesEqual(*term, *other)) {
				total += atoms[j].sign * value;
				like.push_back(j);
			}
		}
		// A group whose factors cancel is left as written: dropping it could empty the
		// side, and `x - x` is the user's to resolve.
		if (like.empty() || total == 0.0) {
			continue;
		}
		const bool whole = total == std::floor(total) && std::fabs(total) < 2147483647.0;
		atoms[i].sign = 1;
		atoms[i].expr = term;
		atoms[i].scale = make_uniq_base<Expression, BoundConstantExpression>(
		    whole ? Value::INTEGER(static_cast<int32_t>(total)) : Value::DOUBLE(total));
		atoms[i].scale_divides = false;
		for (auto it = like.rbegin(); it != like.rend(); ++it) {
			atoms.erase(atoms.begin() + static_cast<int64_t>(*it));
		}
		merged = true;
	}
	return merged;
}

unique_ptr<Expression> DecideCanonicalizer::BindOp(const string &name, unique_ptr<Expression> left,
                                                   unique_ptr<Expression> right) const {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(left));
	children.push_back(std::move(right));
	return BindOp(name, std::move(children));
}

unique_ptr<Expression> DecideCanonicalizer::BindOp(const string &name, unique_ptr<Expression> operand) const {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(operand));
	return BindOp(name, std::move(children));
}

unique_ptr<Expression> DecideCanonicalizer::BindOp(const string &name,
                                                   vector<unique_ptr<Expression>> children) const {
	FunctionBinder function_binder(context);
	ErrorData error;
	auto result = function_binder.BindScalarFunction(DEFAULT_SCHEMA, name, std::move(children), error);
	if (error.HasError()) {
		throw InternalException("DECIDE canonicalizer failed to bind '%s': %s", name, error.Message());
	}
	return result;
}

unique_ptr<Expression> DecideCanonicalizer::BuildAdditive(const vector<Atom> &atoms, bool negate) const {
	unique_ptr<Expression> acc;
	for (auto &atom : atoms) {
		int sign = negate ? -atom.sign : atom.sign;
		auto term = atom.expr->Copy();
		if (atom.scale) {
			// Re-attach the peeled factor in the one canonical spelling: multiplication
			// with the factor on the LEFT. `SUM(x) * 2`, `2 * SUM(x)` and the cast-laden
			// variants all converge here, which is what lets each downstream consumer
			// match a single shape instead of four.
			term = atom.scale_divides
			           ? BindOp("/", std::move(term), atom.scale->Copy())
			           : BindOp("*", atom.scale->Copy(), std::move(term));
		}
		if (!acc) {
			if (sign >= 0) {
				acc = std::move(term);
			} else {
				// Leading negative term: unary minus, NOT `0 - term`. The spelling
				// matters because the additive spine this pass emits is what downstream
				// walkers decompose, and a synthesized `0` is a term to them. It is not
				// a term to K3, which admits only reducers and row-invariant values, so
				// `price - SUM(x) <= cap` rebuilt as `(0 - SUM(x)) - cap` was rejected by
				// the aggregate extractor for a constant this pass had invented.
				// Negating in place introduces no node the spine has to explain away.
				acc = BindOp("-", std::move(term));
			}
			continue;
		}
		acc = BindOp(sign >= 0 ? "+" : "-", std::move(acc), std::move(term));
	}
	return acc;
}

bool DecideCanonicalizer::IsLoneNegatedReducer(const vector<Atom> &atoms) const {
	return atoms.size() == 1 && atoms[0].sign < 0 &&
	       UnwrapDecideCasts(*atoms[0].expr, decide_index)->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE;
}

bool DecideCanonicalizer::IsSignedSpine(const Expression &side, const vector<Atom> &atoms) const {
	auto root = UnwrapDecideCasts(side, decide_index);
	if (atoms.size() != 1 || root->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &name = root->Cast<BoundFunctionExpression>().function.name;
	return name == "+" || name == "-";
}

void DecideCanonicalizer::FoldNegationIntoFactor(Atom &atom) const {
	atom.scale = atom.scale ? BindOp("-", std::move(atom.scale))
	                        : make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(-1));
	atom.sign = -atom.sign;
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeComparison(const Expression &expr) const {
	auto desugared = DesugarNorms(expr);
	return CanonicalizeComparisonInternal(desugared ? *desugared : expr);
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeComparisonInternal(const Expression &expr) const {
	DecideProfileScope profile("canonicalize.comparison");
	auto &cmp = expr.Cast<BoundComparisonExpression>();
	// Two spellings are fixed here. A lone negated reducer carries its sign in its factor,
	// the one spelling a factor on a reducer has, so `-max(e) by (g)` reads as
	// `-2 * max(e) by (g)` does: one MIN/MAX under a factor whose sign is known; and
	// `-(-max(e))` as `max(e)`. The MIN/MAX rewrite routes on the tree it is given, so the
	// tree must be the one its atoms rebuild. And a tuple term may stand under a
	// row-varying factor beside a reducer (PeelScale): the VALUE shape takes it.
	vector<Atom> left_atoms;
	vector<Atom> right_atoms;
	Decompose(*cmp.left, 1, left_atoms, Clause::CONSTRAINT);
	Decompose(*cmp.right, 1, right_atoms, Clause::CONSTRAINT);
	// Like terms under factors known before the query runs are one term: `x + x` is
	// `2 * x`, `min(x) + min(x)` is `2 * min(x)`, so a bound written as a repeated term
	// is read as the bound it is. Merged here so a side that is otherwise canonical does
	// not take the early return below with its twins intact (a merged atom carries a
	// scale, which forces the rebuild), and again on the partitioned decision side,
	// where a term that crossed the relation meets its twin (`3 * x >= x - 4`).
	MergeLikeAtoms(left_atoms);
	MergeLikeAtoms(right_atoms);

	auto Has = [](const vector<Atom> &atoms, Placement placement) {
		for (auto &atom : atoms) {
			if (atom.placement == placement) {
				return true;
			}
		}
		return false;
	};

	bool left_has_decision = Has(left_atoms, Placement::LEFT);
	bool right_has_decision = Has(right_atoms, Placement::LEFT);

	auto HasScale = [](const vector<Atom> &atoms) {
		for (auto &atom : atoms) {
			if (atom.scale) {
				return true;
			}
		}
		return false;
	};

	// Already canonical: decision terms on the left, none on the right, and no
	// plain data stranded on the left. A peeled scale still forces a rebuild -- the
	// side may be correctly partitioned and yet spell the factor on the wrong side
	// (`SUM(x) * 2`), and the whole point of peeling is that consumers see one
	// spelling. Rebuilding an already-canonical `2 * SUM(x)` reproduces it.
	if (left_has_decision && !right_has_decision && !Has(left_atoms, Placement::RIGHT) && !HasScale(left_atoms) &&
	    !(IsLoneNegatedReducer(left_atoms) || IsSignedSpine(*cmp.left, left_atoms))) {
		auto copy = cmp.Copy();
		return FinalizeBoundProvenance(unique_ptr_cast<Expression, BoundComparisonExpression>(std::move(copy)));
	}

	// No decision content anywhere. Not a shape this pass owns; leave it exactly as
	// it is and let the existing validation speak.
	if (!left_has_decision && !right_has_decision) {
		return cmp.Copy();
	}

	// Mirror image: every decision term sits on the right. Swap the sides and flip
	// the relation rather than negating both sides. Signs are preserved, which is
	// what keeps MIN/MAX easy-vs-hard classification in RewriteMinMaxInConstraint
	// stable -- negating a reducer would silently turn a cheap per-row rewrite into
	// a Big-M encoding.
	if (!left_has_decision) {
		auto swapped = make_uniq<BoundComparisonExpression>(FlipComparisonExpression(cmp.type),
		                                                    cmp.right->Copy(), cmp.left->Copy());
		swapped->SetAlias(cmp.GetAlias());
		return CanonicalizeComparison(*swapped);
	}

	// Decision terms on BOTH sides. Migrating the right-hand ones left negates
	// them, which is only sound because the two downstream analyses that used to
	// classify by syntactic position now consult sign: ABS Big-M pinning
	// (ClassifyAbsConstraints) and composed MIN/MAX easy-vs-hard (WalkComposedLhs).
	// Both were made sign-aware before this branch was allowed to fall through.

	// General partition: decision terms migrate left, everything else migrates
	// right. A term that crosses the relation flips sign.
	vector<Atom> new_left;
	vector<Atom> new_right;
	// Crossing the relation flips the sign and nothing else -- the peeled scale
	// travels with its term, since negating `2 * SUM(x)` negates the product, not
	// the factor. Each atom lands in exactly one destination, so it is moved rather
	// than copied; the scale it owns has no second home.
	auto cross = [](Atom &atom) {
		atom.sign = -atom.sign;
		return std::move(atom);
	};
	for (auto &atom : left_atoms) {
		if (atom.placement == Placement::RIGHT) {
			new_right.push_back(cross(atom));
		} else {
			new_left.push_back(std::move(atom));
		}
	}
	for (auto &atom : right_atoms) {
		if (atom.placement == Placement::LEFT) {
			new_left.push_back(cross(atom));
		} else {
			new_right.push_back(std::move(atom));
		}
	}

	MergeLikeAtoms(new_left);
	if (IsLoneNegatedReducer(new_left)) {
		FoldNegationIntoFactor(new_left[0]);
	}
	auto new_lhs = BuildAdditive(new_left, false);
	auto new_rhs = BuildAdditive(new_right, false);
	if (!new_rhs) {
		new_rhs = make_uniq<BoundConstantExpression>(Value::DOUBLE(0));
	}

	// K5: comparison tags are an out-of-band channel and must survive rebuilding.
	// Bound provenance is recomputed semantically from the complete canonical RHS;
	// it is never copied from whichever side happened to be the original RHS.
	auto result = make_uniq<BoundComparisonExpression>(cmp.type, std::move(new_lhs), std::move(new_rhs));
	result->SetAlias(cmp.GetAlias());
	return FinalizeBoundProvenance(std::move(result));
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeObjective(const Expression &objective,
                                                                  double &out_constant_offset) const {
	auto desugared = DesugarNorms(objective);
	return CanonicalizeObjectiveInternal(desugared ? *desugared : objective, out_constant_offset);
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeObjectiveInternal(const Expression &objective,
                                                                          double &out_constant_offset) const {
	DecideProfileScope profile("canonicalize.objective");
	// A `when` or `per` wrapper carries no algebra: recurse into the objective child and copy
	// the condition or grouping columns unchanged. Identical rule to C0, and the same
	// wrapper predicate, so the two clauses cannot drift apart on what a wrapper is.
	if (objective.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conjunction = objective.Cast<BoundConjunctionExpression>();
		if (IsConstraintWrapper(conjunction) && !conjunction.children.empty()) {
			auto result = make_uniq<BoundConjunctionExpression>(conjunction.type);
			result->children.push_back(CanonicalizeObjectiveInternal(*conjunction.children[0], out_constant_offset));
			for (idx_t i = 1; i < conjunction.children.size(); i++) {
				result->children.push_back(conjunction.children[i]->Copy());
			}
			result->SetAlias(conjunction.GetAlias());
			return std::move(result);
		}
	}

	// No decision content anywhere. Not a shape this pass owns -- and a legitimate
	// input rather than an error: RewriteComposedMinMaxObjectiveTop installs a
	// constant placeholder objective and supplies the coefficients from
	// composed_minmax_objective_terms instead.
	if (!ReferencesDecideVar(objective)) {
		return objective.Copy();
	}

	vector<Atom> atoms;
	Decompose(objective, 1, atoms, Clause::OBJECTIVE);
	MergeLikeAtoms(atoms);

	vector<Atom> decision_atoms;
	for (auto &atom : atoms) {
		if (atom.placement == Placement::LEFT) {
			decision_atoms.push_back(std::move(atom));
			continue;
		}
		// A decision-free additive term shifts the objective without moving its
		// argmax/argmin, so it is peeled here and the body downstream is pure decision
		// algebra. Peeling it requires knowing its value, which is exactly the
		// foldable case; a row-varying or query-executed term has no single value to
		// fold and does not belong in an objective at all. A factor distributed onto
		// it (`2 * (sum(x) - 1)`) is part of its value.
		double value = 0;
		unique_ptr<Expression> scaled;
		if (atom.scale) {
			scaled = atom.scale_divides ? BindOp("/", atom.expr->Copy(), atom.scale->Copy())
			                            : BindOp("*", atom.scale->Copy(), atom.expr->Copy());
		}
		if (!TryEvaluateFoldableDouble(context, scaled ? *scaled : *atom.expr, value, FOLD_PLAIN)) {
			throw BinderException("MAXIMIZE/MINIMIZE: '%s' does not reference a decision, so it cannot be part of "
			                      "the objective. Remove it, or move it to the SELECT list.",
			                      QuoteAsWritten(*atom.expr));
		}
		out_constant_offset += atom.sign * value;
	}

	// Every term was decision-free despite the subtree containing a decision
	// reference -- only reachable if Classify and ReferencesDecideVar disagree, which
	// is an engine bug rather than a user error.
	if (decision_atoms.empty()) {
		throw InternalException(
		    "DECIDE canonicalizer: objective references a decision but decomposed into no "
		    "decision-bearing term");
	}
	auto result = BuildAdditive(decision_atoms, false);
	// A reducer under a Boolean decision takes the VALUE shape, as a comparison does: the value rewrite holds the
	// reducer in a helper decision, and the product with it is a Boolean product.
	if (std::any_of(decision_atoms.begin(), decision_atoms.end(),
	                [&](const Atom &atom) { return atom.scale && ReferencesDecideVar(*atom.scale); })) {
		auto alias = result->GetAlias();
		AddDecideTag(alias, VALUE_REWRITE_TAG);
		result->SetAlias(std::move(alias));
	}
	return result;
}

string DecideCanonicalizer::Quote(const Expression &expr) const {
	return message_names ? RenderDecideSource(expr, *message_names) : UserFacingName(expr);
}

string DecideCanonicalizer::QuoteAsWritten(const Expression &expr) const {
	// A node whose written text was recorded (a subquery or a cast) carries it in a tag,
	// and only that text names it in SQL; a reducer is spelled as the plan prints it,
	// with its `when` and `by`.
	idx_t fragment_id;
	auto *node = StripCastsForIdentity(expr);
	return TryParseSourceFragmentTag(node->GetAlias(), fragment_id) ||
	               node->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE
	           ? Quote(expr)
	           : UserFacingName(expr);
}

DecideCanonicalizer::ScopedShape DecideCanonicalizer::ClassifyScoped(const BoundComparisonExpression &cmp,
                                                                     idx_t &group_key, string &refusal) const {
	group_key = DConstants::INVALID_INDEX;
	vector<Atom> lhs_atoms;
	Decompose(*cmp.left, 1, lhs_atoms, Clause::CONSTRAINT);
	bool has_reducer = false;
	for (auto &atom : lhs_atoms) {
		has_reducer = has_reducer || ContainsReducer(*atom.expr);
	}
	if (!has_reducer) {
		// A data-only reducer sits on the right, as a bound.
		return ScopedShape::ROWS;
	}
	// The aggregate shape is today's grouped row: decision reducers sharing one `by` key,
	// under whole-query factors, with only query-wide terms beside them. Anything else on
	// the decision side takes the value rewrite (VALUE): a per-row or keyed decision, a
	// factor that varies by row, a second `by` key, a reducer inside another expression,
	// a product of decisions beside a reducer.
	bool value = false;
	bool first_reducer = true;
	// A query-wide decision beside the reducers counts once per group, in a linear,
	// bilinear or quadratic row alike.
	const Expression *query_wide_decision = nullptr;
	bool nonlinear_beside = false;
	// `<>` excludes an AVG's value by scaling the bound with the number of rows in the
	// group, which is exact only when every term is an AVG reading all of them.
	bool has_avg = false;
	bool only_plain_avgs = true;
	for (auto &atom : lhs_atoms) {
		if (!ContainsReducer(*atom.expr)) {
			only_plain_avgs = false;
			nonlinear_beside = nonlinear_beside || !DecideExpressionDegree(*atom.expr, decide_index).IsLinear();
			if (IsQueryWideExpression(*atom.expr)) {
				if (ReferencesDecideVar(*atom.expr)) {
					query_wide_decision = atom.expr;
				}
				continue;
			}
			value = true; // a per-row or keyed decision, or row data, beside a reducer
			continue;
		}
		auto *root = UnwrapDecideCasts(*atom.expr, decide_index);
		if (root->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE || !ReferencesDecideVar(*root)) {
			only_plain_avgs = false;
			value = true; // a reducer inside another expression: `abs(sum(e) by (g) - 40)`
			continue;
		}
		auto &reducer = root->Cast<BoundAggregateExpression>();
		idx_t qualifier;
		bool is_avg =
		    StringUtil::CIEquals(reducer.function.name, "avg") || HasDecideTag(reducer.GetAlias(), AVG_REWRITE_TAG);
		has_avg = has_avg || is_avg;
		only_plain_avgs =
		    only_plain_avgs && is_avg && !reducer.filter && !TryParseQualifiedReducerTag(reducer.GetAlias(), qualifier);
		if (atom.scale && !IsQueryWideConstant(*atom.scale)) {
			value = true; // a factor that varies by row: a tuple term of the row's constraint
		}
		idx_t key = DConstants::INVALID_INDEX;
		TryParseGroupKeyTag(root->GetAlias(), key);
		if (first_reducer) {
			group_key = key;
			first_reducer = false;
		} else if (key != group_key) {
			value = true; // reducers with different `by` keys
		}
	}
	if (query_wide_decision && nonlinear_beside) {
		value = true; // `sum(x) + b * s`: the product beside the reducer is a term of each row
	}
	if (cmp.type == ExpressionType::COMPARE_NOTEQUAL && has_avg && (!only_plain_avgs || nonlinear_beside)) {
		refusal = "DECIDE: AVG in a <> constraint is not supported yet beside another term or with its own when "
		          "or per";
		return ScopedShape::UNSUPPORTED;
	}
	// A MIN or MAX beside another term, or under a factor of unknown sign, takes the
	// composed formulation where that applies: one row over all of the clause's kept rows,
	// under one bound, with nothing but reducers in it. With a `by`, a bound that is not
	// a constant, or a decision beside it, the value rewrite pins the extremum per group.
	if (!value && TakesComposedMinMax(lhs_atoms) &&
	    (group_key != DConstants::INVALID_INDEX || !cmp.right->IsFoldable() || query_wide_decision)) {
		value = true;
	}
	return value ? ScopedShape::VALUE : ScopedShape::AGGREGATE;
}

bool DecideCanonicalizer::TakesComposedMinMax(const vector<Atom> &atoms) const {
	bool has_minmax = false;
	for (auto &atom : atoms) {
		auto *root = UnwrapDecideCasts(*atom.expr, decide_index);
		if (root->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE && ReferencesDecideVar(*root)) {
			auto name = StringUtil::Lower(root->Cast<BoundAggregateExpression>().function.name);
			has_minmax = has_minmax || name == "min" || name == "max";
		}
	}
	return has_minmax &&
	       (atoms.size() > 1 || ScaleSignAtPlanTime(context, atoms[0].scale.get(), atoms[0].scale_divides) == 0);
}

CanonicalConstraintClass DecideCanonicalizer::ClassifyCanonicalComparison(const Expression &expr) const {
	auto &cmp = expr.Cast<BoundComparisonExpression>();
	if (ReferencesDecideVar(*cmp.right)) {
		return CanonicalConstraintClass::INVALID;
	}
	idx_t group_key;
	string refusal;
	switch (ClassifyScoped(cmp, group_key, refusal)) {
	case ScopedShape::ROWS:
		return CanonicalConstraintClass::PER_ROW;
	case ScopedShape::AGGREGATE:
		return CanonicalConstraintClass::AGGREGATE;
	case ScopedShape::VALUE:
		return CanonicalConstraintClass::VALUE;
	default:
		return CanonicalConstraintClass::INVALID;
	}
}

void DecideCanonicalizer::ValidateCanonicalComparison(const BoundComparisonExpression &comparison) const {
	auto classification = ClassifyCanonicalComparison(comparison);
	if (classification == CanonicalConstraintClass::INVALID) {
		if (ReferencesDecideVar(*comparison.right)) {
			throw InternalException("DECIDE canonicalizer: a decision survived on the bound side of '%s'",
			                        comparison.ToString());
		}
		idx_t group_key;
		string refusal;
		ClassifyScoped(comparison, group_key, refusal);
		throw BinderException(refusal);
	}
	// A foldable bound is the one case whose NULL result is knowable at planning
	// time. Do not execute subqueries or speculate about nullable data columns here;
	// those retain the runtime COALESCE guidance.
	if (comparison.right->IsFoldable()) {
		Value value;
		if (ExpressionExecutor::TryEvaluateScalar(context, *comparison.right, value) && value.IsNull()) {
			throw BinderException(
			    "DECIDE constraint bound evaluates to NULL. Use COALESCE(...) to provide a numeric bound.");
		}
	}
}

void DecideCanonicalizer::ValidateCanonicalTree(const Expression &constraints) const {
	auto validate = [&](const Expression &expression) {
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			ValidateCanonicalComparison(expression.Cast<BoundComparisonExpression>());
			return true;
		}
		return true;
	};
	VisitConstraintTree(constraints, validate);
}

void DecideCanonicalizer::VerifyCanonicalComparison(const BoundComparisonExpression &comparison) const {
	if (!comparison.left || !comparison.right) {
		CanonicalInvariantFailure("C1", "comparison is missing an operand", comparison);
	}
	if (!ReferencesDecideVar(*comparison.left)) {
		CanonicalInvariantFailure("C1", "comparison has no decision-bearing left side", comparison);
	}
	if (ReferencesDecideVar(*comparison.right)) {
		CanonicalInvariantFailure("C2", "right side still references a DECIDE variable", comparison);
	}

	vector<Atom> lhs_atoms;
	vector<Atom> rhs_atoms;
	Decompose(*comparison.left, 1, lhs_atoms, Clause::CONSTRAINT);
	Decompose(*comparison.right, 1, rhs_atoms, Clause::CONSTRAINT);
	for (auto &atom : lhs_atoms) {
		if (atom.placement != Placement::LEFT) {
			CanonicalInvariantFailure("C3", "decision-free additive term remains on the left", comparison);
		}
	}
	for (auto &atom : rhs_atoms) {
		if (atom.placement != Placement::RIGHT) {
			CanonicalInvariantFailure("C3", "decision-bearing additive term remains on the right", comparison);
		}
	}
	if (ClassifyCanonicalComparison(comparison) == CanonicalConstraintClass::INVALID) {
		CanonicalInvariantFailure("C5", "comparison is neither a valid per-row nor aggregate constraint",
		                          comparison);
	}
}

//! C0's scope rule: under a wrapper there is one clause, and a `when` holds no other `when`, so every walk that
//! carries the enclosing scope (DecideEnclosingScope) reads one scope per clause.
static void VerifyOneScopePerClause(const Expression &expression, bool under_wrapper, bool under_when) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return;
	}
	auto &conjunction = expression.Cast<BoundConjunctionExpression>();
	if (!IsConstraintWrapper(conjunction) && under_wrapper) {
		CanonicalInvariantFailure("C0", "a plain AND under a WHEN or per wrapper", expression);
	}
	if (IsWhenConstraintWrapper(conjunction) && under_when) {
		CanonicalInvariantFailure("C0", "a WHEN wrapper inside another WHEN wrapper", expression);
	}
	const bool wrapper = IsConstraintWrapper(conjunction);
	const bool when = under_when || IsWhenConstraintWrapper(conjunction);
	for (idx_t i = 0; i < conjunction.children.size(); i++) {
		if (IsConstraintChild(conjunction, i)) {
			VerifyOneScopePerClause(*conjunction.children[i], under_wrapper || wrapper, when);
		}
	}
}

void DecideCanonicalizer::VerifyCanonicalTree(const Expression &constraints) const {
	auto verify = [&](const Expression &expression) {
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			VerifyCanonicalComparison(expression.Cast<BoundComparisonExpression>());
			return true;
		}
		if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
			// Optimizer-owned rewrites may replace a fully extracted constraint with a
			// Boolean placeholder. C0 deliberately preserves non-comparison leaves.
			return true;
		}

		auto &conjunction = expression.Cast<BoundConjunctionExpression>();
		if (conjunction.type != ExpressionType::CONJUNCTION_AND) {
			CanonicalInvariantFailure("C0", "constraint conjunction is not AND", expression);
		}
		if (IsWhenConstraintWrapper(conjunction) && conjunction.children.size() != 2) {
			CanonicalInvariantFailure("C0", "WHEN wrapper must contain one constraint and one condition",
			                          expression);
		}
		if (IsPerClassWrapper(conjunction) && conjunction.children.empty()) {
			CanonicalInvariantFailure("C0", "per-class wrapper must contain a constraint", expression);
		}
		return true;
	};
	VisitConstraintTree(constraints, verify);
}

void DecideCanonicalizer::VerifyCanonicalObjectiveBody(const Expression &objective) const {
	if (objective.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conjunction = objective.Cast<BoundConjunctionExpression>();
		if (IsConstraintWrapper(conjunction)) {
			if (conjunction.children.empty()) {
				CanonicalInvariantFailure("O0", "objective wrapper has no objective child", objective);
			}
			VerifyCanonicalObjectiveBody(*conjunction.children[0]);
			return;
		}
	}
	// A decision-free objective is the composed MIN/MAX placeholder, which supplies
	// its coefficients from composed_minmax_objective_terms instead of from the tree.
	if (!ReferencesDecideVar(objective)) {
		return;
	}
	vector<Atom> atoms;
	Decompose(objective, 1, atoms, Clause::OBJECTIVE);
	for (auto &atom : atoms) {
		if (atom.placement != Placement::LEFT) {
			CanonicalInvariantFailure("O2", "canonical objective retains a decision-free additive term",
			                          *atom.expr);
		}
	}
}

void DecideCanonicalizer::VerifyCanonicalObjective(const Expression &objective) const {
#ifdef DEBUG
	try {
		if (ContainsDesugarableNorm(objective, decide_index)) {
			CanonicalInvariantFailure("C8", "a norm with an expression for a definition was not replaced by it",
			                          objective);
		}
		VerifyCanonicalObjectiveBody(objective);
		if (!ReferencesDecideVar(objective)) {
			return;
		}
		// Re-canonicalizing a canonical objective must peel nothing: every additive
		// constant was already folded into objective_constant_offset, so a nonzero
		// residual means an offset would be counted twice.
		double residual_offset = 0.0;
		auto fixed_point = CanonicalizeObjective(objective, residual_offset);
		if (residual_offset != 0.0) {
			CanonicalInvariantFailure("O2", "canonical objective still yields a constant offset", objective);
		}
		if (!CanonicalTreesEqual(objective, *fixed_point)) {
			CanonicalInvariantFailure(
			    "O4/O7", "canonicalizing the objective again changes its structure, reducer scale, or tags",
			    objective);
		}
	} catch (const InternalException &) {
		throw;
	} catch (const std::exception &ex) {
		throw InternalException("DECIDE canonical objective verification failed for %s: %s",
		                        objective.ToString(), ex.what());
	}
#endif
}

void DecideCanonicalizer::VerifyCanonical(const Expression &constraints) const {
	// One test per clause, so it runs in every build: each walk that carries a clause's scope relies on it.
	VerifyOneScopePerClause(constraints, false, false);
#ifdef DEBUG
	try {
		if (ContainsDesugarableNorm(constraints, decide_index)) {
			CanonicalInvariantFailure("C8", "a norm with an expression for a definition was not replaced by it",
			                          constraints);
		}
		VerifyCanonicalTree(constraints);
		auto fixed_point = CanonicalizeTreeInternal(constraints);
		if (!CanonicalTreesEqual(constraints, *fixed_point)) {
			CanonicalInvariantFailure(
			    "C4/C6/C7",
			    "canonicalizing the tree again changes its structure, reducer scale, or provenance tags",
			    constraints);
		}
	} catch (const InternalException &) {
		throw;
	} catch (const std::exception &ex) {
		throw InternalException("DECIDE canonical invariant verification failed for %s: %s",
		                        constraints.ToString(), ex.what());
	}
#endif
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeTreeInternal(const Expression &constraints) const {
	if (constraints.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = constraints.Cast<BoundConjunctionExpression>();

		auto result = make_uniq<BoundConjunctionExpression>(conj.type);
		result->SetAlias(conj.GetAlias());
		for (idx_t i = 0; i < conj.children.size(); i++) {
			// A `when` or `per` wrapper holds the constraint in child 0; the remaining
			// children are its condition / key columns and are not constraints.
			result->children.push_back(IsConstraintChild(conj, i) ? CanonicalizeTreeInternal(*conj.children[i])
			                                                      : conj.children[i]->Copy());
		}
		// `per K:` over a clause in the aggregate shape adds no row to the model: its rows
		// are one per `by` group of the kept rows, and every per-K constraint in a group
		// differs only in its bound, which the group's tightest bound covers once K
		// determines it (a scope check). The wrapper stays all the same: it names the
		// classes each group's row stands for, which a diagnosis reports one by one
		// (language/guide.md §8.1, D28); the linear form reads it as the clause's class key.
		return std::move(result);
	}
	if (constraints.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto result = CanonicalizeComparisonInternal(constraints);
		// A clause in the aggregate shape is grouped by the `by` key its decision reducers
		// share; the comparison carries it, as later rewrites rebuild the reducers.
		if (!ReferencesDecideVar(*result->Cast<BoundComparisonExpression>().right)) {
			idx_t group_key;
			string refusal;
			auto shape = ClassifyScoped(result->Cast<BoundComparisonExpression>(), group_key, refusal);
			idx_t stamped;
			if (shape == ScopedShape::AGGREGATE && group_key != DConstants::INVALID_INDEX &&
			    !TryParseGroupKeyTag(result->GetAlias(), stamped)) {
				auto alias = result->GetAlias();
				AddDecideTag(alias, MakeGroupKeyTag(group_key));
				result->SetAlias(std::move(alias));
			}
			// The VALUE shape is decided here, once: the value rewrite reads the tag.
			if (shape == ScopedShape::VALUE && !HasDecideTag(result->GetAlias(), VALUE_REWRITE_TAG)) {
				auto alias = result->GetAlias();
				AddDecideTag(alias, VALUE_REWRITE_TAG);
				result->SetAlias(std::move(alias));
			}
		}
		return result;
	}
	return constraints.Copy();
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeTree(const Expression &constraints) const {
	auto desugared = DesugarNorms(constraints);
	auto result = CanonicalizeTreeInternal(desugared ? *desugared : constraints);
	ValidateCanonicalTree(*result);
	return result;
}

void DecideCanonicalizer::BuildConstraintScopeChecks(const Expression &constraints, DecideScopeChecks &scope) const {
	ForEachScopedConstraintLeaf(constraints, [&](const Expression &leaf, const DecideEnclosingScope &enclosing) {
		BuildClauseScopeChecks(leaf, enclosing, scope);
	});
}

//===--------------------------------------------------------------------===//
// Scope checks (language/guide.md §7)
//===--------------------------------------------------------------------===//

namespace {
//! What a clause reads once per class: its tuple terms -- data values and decisions
//! outside reducers -- and its reducers, whose bodies are read per group instead.
struct ClauseParts {
	vector<const Expression *> values;
	vector<const BoundColumnRefExpression *> decisions;
	vector<const BoundAggregateExpression *> reducers;
};
} // namespace

static bool HoldsReducer(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { found = found || HoldsReducer(child); });
	return found;
}

//! Fills `parts` from `expr`. A value is a maximal subtree with neither a decision nor a
//! reducer; a literal or a query-wide value has one value everywhere and is left out.
static void CollectClauseParts(const Expression &expr, idx_t decide_index,
                               const std::function<bool(const Expression &)> &query_wide, ClauseParts &parts) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		parts.reducers.push_back(&expr.Cast<BoundAggregateExpression>());
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	    expr.Cast<BoundColumnRefExpression>().binding.table_index == decide_index) {
		parts.decisions.push_back(&expr.Cast<BoundColumnRefExpression>());
		return;
	}
	if (!BoundExpressionReferencesDecide(expr, decide_index) && !HoldsReducer(expr)) {
		if (!expr.IsFoldable() && !query_wide(expr)) {
			parts.values.push_back(&expr);
		}
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { CollectClauseParts(child, decide_index, query_wide, parts); });
}

//! The key of a scope check over `key` within each group of `by`: their union, added to
//! `keys` when no key holds it yet. INVALID_INDEX is the empty key.
static idx_t WithinGroups(idx_t key, idx_t by, vector<DecideKey> &keys) {
	if (key == DConstants::INVALID_INDEX || by == DConstants::INVALID_INDEX) {
		return key == DConstants::INVALID_INDEX ? by : key;
	}
	vector<DecideKeyElement> elements;
	for (auto index : {key, by}) {
		for (auto &element : keys[index].elements) {
			elements.push_back(element.Copy());
		}
	}
	auto text = keys[key].text + ", " + keys[by].text;
	return FindOrCreateDecideKey(std::move(elements), text, keys);
}

//! How a message describes a decision's own key: `declared per row`, `declared per
//! D.depotID`.
static string DeclaredScope(const DecideVarScopeInfo &scope) {
	if (scope.IsKeyed()) {
		return "declared per " + scope.declared_key;
	}
	return "declared per row";
}

void DecideCanonicalizer::BuildReducerScopeChecks(const BoundAggregateExpression &reducer, const Expression *rows,
                                                  DecideScopeChecks &scope) const {
	DecideSourceNames names {scope.source_fragments, scope.entity_scopes, scope.keys};
	auto reducer_text = RenderDecideReducer(reducer, names);
	idx_t by = DConstants::INVALID_INDEX;
	TryParseGroupKeyTag(reducer.GetAlias(), by);
	// The reducer's own `per K'` -- or `D` of the colon form `SUM(D: e)` -- determines its
	// body within each `by` group, over the rows its `when` keeps.
	idx_t scope_idx;
	if (!TryParseQualifiedReducerTag(reducer.GetAlias(), scope_idx) || scope_idx >= scope.entity_scopes.size() ||
	    reducer.children.empty()) {
		return;
	}
	auto key = WithinGroups(scope.entity_scopes[scope_idx].key, by, scope.keys);
	auto subject = DecideKeyText(scope.keys, scope.entity_scopes[scope_idx].key);
	ClauseParts body;
	CollectClauseParts(
	    *reducer.children[0], decide_index, [&](const Expression &expr) { return IsQueryWideConstant(expr); }, body);
	auto make_check = [&]() {
		DecideScopeCheck check;
		check.kind = DecideScopeCheckKind::DETERMINES;
		check.rows = rows ? rows->Copy() : nullptr;
		check.key = key;
		check.filter = reducer.filter ? reducer.filter->Copy() : nullptr;
		check.subject = subject;
		check.context = "in " + reducer_text;
		return check;
	};
	for (auto *value : body.values) {
		auto check = make_check();
		check.value = value->Copy();
		check.object = RenderDecideSource(*value, names);
		scope.checks.push_back(std::move(check));
	}
	for (auto *decision : body.decisions) {
		auto var_idx = decision->binding.column_index;
		if (var_idx < variable_scopes.size() && variable_scopes[var_idx].IsScalar()) {
			continue;
		}
		auto check = make_check();
		check.decision = var_idx;
		check.object = decision->GetName();
		scope.checks.push_back(std::move(check));
	}
}

void DecideCanonicalizer::BuildClauseScopeChecks(const Expression &leaf, const DecideEnclosingScope &enclosing,
                                                 DecideScopeChecks &scope) const {
	DecideSourceNames names {scope.source_fragments, scope.entity_scopes, scope.keys};
	ClauseParts parts;
	CollectClauseParts(
	    leaf, decide_index, [&](const Expression &expr) { return IsQueryWideConstant(expr); }, parts);
	// The `per` key determines every tuple term, decision and `by` key of the clause.
	if (enclosing.per_wrapper) {
		auto &per = *enclosing.per_wrapper;
		auto key = enclosing.PerKey();
		idx_t fragment;
		auto key_text = TryParseSourceFragmentTag(per.GetAlias(), fragment) && fragment < scope.source_fragments.size()
		                    ? scope.source_fragments[fragment]
		                    : DecideKeyText(scope.keys, key);
		auto make_check = [&]() {
			DecideScopeCheck check;
			check.kind = DecideScopeCheckKind::DETERMINES;
			check.rows = enclosing.Condition() ? enclosing.Condition()->Copy() : nullptr;
			check.key = key;
			check.subject = key_text;
			return check;
		};
		for (auto *value : parts.values) {
			auto check = make_check();
			check.value = value->Copy();
			check.object = RenderDecideSource(*value, names);
			scope.checks.push_back(std::move(check));
		}
		for (auto *decision : parts.decisions) {
			auto var_idx = decision->binding.column_index;
			if (var_idx < variable_scopes.size() && variable_scopes[var_idx].IsScalar()) {
				continue;
			}
			auto check = make_check();
			check.decision = var_idx;
			check.object = decision->GetName();
			check.context =
			    var_idx < variable_scopes.size() ? DeclaredScope(variable_scopes[var_idx]) : string("declared per row");
			scope.checks.push_back(std::move(check));
		}
		for (auto *reducer : parts.reducers) {
			idx_t group_key;
			if (!TryParseGroupKeyTag(reducer->GetAlias(), group_key) || group_key >= scope.keys.size()) {
				continue;
			}
			auto &by = scope.keys[group_key];
			for (auto &element : by.elements) {
				auto check = make_check();
				check.value = element.Grouping().Copy();
				check.object = element.name;
				check.context = "in by (" + by.text + ")";
				scope.checks.push_back(std::move(check));
			}
		}
	}
	for (auto *reducer : parts.reducers) {
		BuildReducerScopeChecks(*reducer, enclosing.Condition(), scope);
	}
}

void DecideCanonicalizer::BuildObjectiveScopeChecks(const Expression &objective, DecideScopeChecks &scope) const {
	// The two-level form's PER wrapper and a `when` in front; the body is produced once.
	const Expression *body = &objective;
	DecideEnclosingScope enclosing;
	bool two_level = false;
	while (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = body->Cast<BoundConjunctionExpression>();
		if (!IsConstraintWrapper(conj) || conj.children.empty()) {
			break;
		}
		two_level = two_level || IsPerConstraintWrapper(conj);
		enclosing = EnterDecideScope(conj, enclosing);
		body = conj.children[0].get();
	}
	DecideSourceNames names {scope.source_fragments, scope.entity_scopes, scope.keys};
	if (two_level) {
		return; // its classes are the inner reducer's own `by` groups, its key's
	}
	ClauseParts parts;
	CollectClauseParts(
	    *body, decide_index, [&](const Expression &expr) { return IsQueryWideConstant(expr); }, parts);
	auto make_check = [&]() {
		DecideScopeCheck check;
		check.kind = DecideScopeCheckKind::DETERMINES;
		check.rows = enclosing.Condition() ? enclosing.Condition()->Copy() : nullptr;
		check.subject = "()";
		return check;
	};
	// Produced once (`per ()`): every value and decision outside a reducer has one value or
	// instance, and every top-level reducer one `by` group, over the kept rows.
	for (auto *value : parts.values) {
		auto check = make_check();
		check.value = value->Copy();
		check.object = RenderDecideSource(*value, names);
		scope.checks.push_back(std::move(check));
	}
	for (auto *decision : parts.decisions) {
		auto var_idx = decision->binding.column_index;
		if (var_idx < variable_scopes.size() && variable_scopes[var_idx].IsScalar()) {
			continue;
		}
		auto check = make_check();
		check.decision = var_idx;
		check.object = decision->GetName();
		check.context =
		    var_idx < variable_scopes.size() ? DeclaredScope(variable_scopes[var_idx]) : string("declared per row");
		scope.checks.push_back(std::move(check));
	}
	for (auto *reducer : parts.reducers) {
		idx_t group_key;
		if (TryParseGroupKeyTag(reducer->GetAlias(), group_key) && group_key < scope.keys.size()) {
			auto &by = scope.keys[group_key];
			for (auto &element : by.elements) {
				auto check = make_check();
				check.value = element.Grouping().Copy();
				check.object = element.name;
				check.context = "in by (" + by.text + ")";
				scope.checks.push_back(std::move(check));
			}
		}
		BuildReducerScopeChecks(*reducer, enclosing.Condition(), scope);
	}
}

} // namespace duckdb
