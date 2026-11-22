#include "duckdb/common/decide_profile.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"

#include <numeric>

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/decide/decide_product_form.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
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
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
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
	if (!is_mult && !is_div) {
		return false;
	}

	// A reducer, not a frame's marker: a frame is a tuple term, scaled as one.
	auto is_reducer = [&](const Expression &child) {
		auto *node = UnwrapDecideCasts(child, decide_index);
		idx_t frame;
		return node->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE && !TryGetDecideFrame(*node, frame);
	};
	idx_t aggregate_child;
	if (is_div && is_reducer(*func.children[0])) {
		aggregate_child = 0;
	} else if (is_mult && is_reducer(*func.children[1])) {
		aggregate_child = 1;
	} else {
		return false;
	}
	// A scale is read once per row, so it holds no reducer: `(sum(x) + 1) * max(x)` is a product of decisions, left
	// whole for the reader that refuses it.
	if (DecideCanonicalizer::ContainsReducer(*func.children[1 - aggregate_child])) {
		return false;
	}

	result.aggregate =
	    &UnwrapDecideCasts(*func.children[aggregate_child], decide_index)->Cast<BoundAggregateExpression>();
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
		if (func.function.name == "+") {
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
		if (func.function.name == "-") {
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
		(term.IsFoldable() ? split.moved : split.kept).emplace_back(sign, &term);
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

[[noreturn]] static void CanonicalInvariantFailure(const string &rule, const string &detail, const Expression &expr) {
	throw InternalException("DECIDE canonical invariant %s violated: %s. Expression: %s", rule, detail,
	                        expr.ToString());
}

//! The key of each decision's scope: the empty key for a query-wide decision, its key for a keyed one, one per row
//! for every other.
static vector<idx_t> DecisionKeys(const vector<DecideVarScopeInfo> &variable_scopes,
                                  const vector<KeyedScopeInfo> &keyed_scopes) {
	vector<idx_t> keys;
	for (auto &scope : variable_scopes) {
		idx_t key = DECIDE_ROW_KEY;
		if (scope.IsGlobal()) {
			key = DConstants::INVALID_INDEX;
		} else if (scope.IsKeyed()) {
			key = keyed_scopes[scope.keyed_scope_idx].key;
		}
		keys.push_back(key);
	}
	return keys;
}

DecideCanonicalizer::DecideCanonicalizer(ClientContext &context, idx_t decide_index,
                                         vector<DecideVarScopeInfo> variable_scopes_p,
                                         const vector<KeyedScopeInfo> &keyed_scopes_p, const vector<DecideKey> &keys_p,
                                         const vector<bool> &is_boolean)
    : context(context), decide_index(decide_index), variable_scopes(std::move(variable_scopes_p)),
      decision_keys(DecisionKeys(variable_scopes, keyed_scopes_p)), boolean_decisions(is_boolean),
      keyed_scopes(keyed_scopes_p), keys(keys_p) {
}

DecideCanonicalizer::DecideCanonicalizer(ClientContext &context, idx_t decide_index,
                                         vector<DecideVarScopeInfo> variable_scopes_p,
                                         const vector<KeyedScopeInfo> &keyed_scopes_p, const vector<DecideKey> &keys_p,
                                         const vector<bool> &is_boolean, unordered_set<idx_t> query_wide_table_indexes)
    : context(context), decide_index(decide_index), variable_scopes(std::move(variable_scopes_p)),
      decision_keys(DecisionKeys(variable_scopes, keyed_scopes_p)),
      query_wide_table_indexes(std::move(query_wide_table_indexes)), judge_column_refs(true),
      boolean_decisions(is_boolean), keyed_scopes(keyed_scopes_p), keys(keys_p) {
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
		if (DecideExpressionsEqual(*seen, value)) {
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
//! handling only. A nested reducer over decisions is its own reducer and is not looked into;
//! one over data alone is a decision-free part, NULL where its group has no value. A frame is
//! tested whole, over data or decisions: it reads another row, or nothing (D84), so no
//! column of the row itself says whether it has a value.
static void CollectNullableData(const Expression &expr, idx_t decide_index, vector<unique_ptr<Expression>> &out) {
	idx_t frame;
	if (TryGetDecideFrame(expr, frame)) {
		AddNullTest(expr, out);
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE &&
	    BoundExpressionReferencesDecide(expr, decide_index)) {
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
                                      const vector<KeyedScopeInfo> &scopes, const vector<DecideKey> &keys,
                                      vector<unique_ptr<Expression>> &out) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &binding = expr.Cast<BoundColumnRefExpression>().binding;
		auto var = binding.column_index;
		if (binding.table_index != decide_index || variable_scopes[var].scope != DecideVarScope::KEYED) {
			return;
		}
		for (auto &element : DecideKeyElements(keys, scopes[variable_scopes[var].keyed_scope_idx].key)) {
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

void DecideCanonicalizer::SkipNullData(Expression &expr, const vector<KeyedScopeInfo> &scopes,
                                       const vector<DecideKey> &keys) const {
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { SkipNullData(child, scopes, keys); });
	idx_t frame;
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE || !ReferencesDecideVar(expr) ||
	    TryGetDecideFrame(expr, frame)) {
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

//! The one walk behind both questions: whether a reducer `counts` stands anywhere in `expr`. A frame's marker and a
//! definition's body are none (see the header).
template <class COUNTS>
static bool HoldsReducer(const Expression &expr, COUNTS &&counts) {
	idx_t definition_aux;
	if (TryParseDecideIndexTag(expr.GetAlias(), DEFINITION_FUNCTION_TAG_PREFIX, definition_aux)) {
		return false; // the F node owns this body; callers see its scalar value, not its source reducers
	}
	idx_t frame;
	if (TryGetDecideFrame(expr, frame)) {
		return false; // a tuple term read at another row, whose payload holds no reducer (D80)
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE && counts(expr)) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = HoldsReducer(child, counts);
		}
	});
	return found;
}

bool DecideCanonicalizer::ContainsReducer(const Expression &expr) {
	return HoldsReducer(expr, [](const Expression &) { return true; });
}

bool DecideCanonicalizer::ContainsDecisionReducer(const Expression &expr) const {
	return HoldsReducer(expr, [&](const Expression &reducer) { return ReferencesDecideVar(reducer); });
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
		return boolean_decisions[column];
	}
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func = cur->Cast<BoundFunctionExpression>();
	if (func.function.name == "*") {
		// Data times the Boolean. A product of two decisions beside a reducer is degree 3, which the shape check
		// refuses after canonicalization, so reading its first decision here only decides the shape it is refused in.
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
		return query_wide_table_indexes.find(colref.binding.table_index) != query_wide_table_indexes.end();
	}
	if (expr.IsFoldable()) {
		return true;
	}
	// Arithmetic over query-wide values is itself query-wide, which is what makes
	// `2 * (SELECT k FROM p) * SUM(x)` work: neither factor folds on its own.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
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

//! Whether `key` holds `value` by construction, so every class shows it once: `value` is one of its elements as rows
//! group on it (reflexivity, the trivial functional dependency), or a deterministic function of them and of
//! constants (`CAST(cap AS DOUBLE)`, `cap + 1`). No key holds anything for the rows (DECIDE_ROW_KEY) or for the whole
//! query (INVALID_INDEX, whose one class can show several values), and a value that reads no element is not held.
static bool KeyHolds(const vector<DecideKey> &keys, idx_t key, const Expression &value) {
	if (key == DConstants::INVALID_INDEX || key == DECIDE_ROW_KEY) {
		return false;
	}
	for (auto &element : DecideKeyElements(keys, key)) {
		if (element.Grouping().Equals(value) || element.expression->Equals(value)) {
			return true;
		}
	}
	switch (value.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
	case ExpressionClass::BOUND_SUBQUERY:
	case ExpressionClass::BOUND_AGGREGATE:
		return false;
	default:
		break;
	}
	if (value.IsVolatile()) {
		return false;
	}
	bool reads_the_key = false;
	bool held = true;
	ExpressionIterator::EnumerateChildren(value, [&](const Expression &child) {
		if (child.IsFoldable()) {
			return;
		}
		reads_the_key = true;
		held = held && KeyHolds(keys, key, child);
	});
	return reads_the_key && held;
}

idx_t DecideCanonicalizer::ScopeKeyOf(const Expression &expr, idx_t data_key) const {
	if (IsQueryWideValueTag(expr.GetAlias())) {
		return DConstants::INVALID_INDEX;
	}
	if (!ReferencesDecideVar(expr) && KeyHolds(keys, data_key, expr)) {
		return data_key;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (colref.binding.table_index == decide_index) {
			auto var_idx = colref.binding.column_index;
			return decision_keys[var_idx];
		}
		// Data is one value per class only where the `per` key holds it by construction (above): any other value may
		// take several in a class, each a variant of its own (D70).
		return DECIDE_ROW_KEY;
	}
	// A reducer is one value per group of its own `by`: ClassifyScoped reads it as a reducer term, not here.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		return DECIDE_ROW_KEY;
	}
	if (expr.IsFoldable()) {
		return DConstants::INVALID_INDEX;
	}
	idx_t key = DConstants::INVALID_INDEX;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { key = JoinDecideKeys(key, ScopeKeyOf(child, data_key)); });
	return key;
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
		return StringUtil::Upper(DecideWrittenName(agg.GetAlias(), agg.function.name)) + "(" +
		       UserFacingName(*agg.children[0]) + ")";
	}
	auto name = StripDecideTags(cur->GetName());
	return name.empty() ? cur->ToString() : name;
}

unique_ptr<Expression> DecideCanonicalizer::WithoutDecisionNegation(const Expression &factor, bool &negated) const {
	auto &bare = *UnwrapDecideCasts(factor, decide_index);
	if (bare.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return factor.Copy();
	}
	auto &func = bare.Cast<BoundFunctionExpression>();
	if (func.function.name == "-" && func.children.size() == 1 && ReferencesDecideVar(*func.children[0])) {
		negated = !negated;
		return WithoutDecisionNegation(*func.children[0], negated);
	}
	if (func.function.name == "*" && func.children.size() == 2) {
		const bool before = negated;
		auto left = ReferencesDecideVar(*func.children[0]) ? WithoutDecisionNegation(*func.children[0], negated)
		                                                   : func.children[0]->Copy();
		auto right = ReferencesDecideVar(*func.children[1]) ? WithoutDecisionNegation(*func.children[1], negated)
		                                                    : func.children[1]->Copy();
		if (negated != before) {
			return BindOp("*", std::move(left), std::move(right));
		}
	}
	return factor.Copy();
}

string DecideCanonicalizer::DecisionScalesReducerMessage(const Expression &factor, const Expression &positive,
                                                         bool divides) const {
	// A decision column says its type; a compound factor (`b - c`) is one expression over several. A BOOL decision
	// scales a reducer on its own, so a BOOL one refused is on a reducer inside another expression.
	auto &bare = *UnwrapDecideCasts(positive, decide_index);
	string subject = "'" + Quote(factor) + "' reads a decision";
	string object = "a reducer";
	if (bare.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto column = bare.Cast<BoundColumnRefExpression>().binding.column_index;
		const bool boolean = boolean_decisions[column];
		subject = "'" + Quote(factor) + "' is " +
		          (boolean                                   ? "a BOOL"
		           : bare.return_type == LogicalType::DOUBLE ? "a REAL"
		                                                     : "an INT") +
		          " decision";
		if (boolean && !divides) {
			object = "a reducer inside another expression";
		}
	}
	return subject + ", which cannot " + (divides ? "divide" : "scale") + " " + object;
}

const Expression &DecideCanonicalizer::PeelScale(const Expression &expr, unique_ptr<Expression> &out_scale,
                                                 bool &out_divides, bool &out_negated, Clause clause) const {
	out_scale = nullptr;
	out_divides = false;
	out_negated = false;

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
	//! The factors whose minus signs were taken off (WithoutDecisionNegation); `multipliers` points into them.
	vector<unique_ptr<Expression>> without_negation;
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
		if (!is_mult && !is_div) {
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
		if (ReferencesDecideVar(*factor)) {
			// A constraint takes any data factor (a row-varying one is a tuple term, below), and a clause of
			// either kind a Boolean decision on a bare reducer: the clause takes the VALUE shape, where the
			// reducer becomes a helper decision and the product `b * helper` is linearized as every Boolean
			// product is. An INT or REAL decision is not convex, and no decision divides.
			bool factor_negated = false;
			auto positive_factor = WithoutDecisionNegation(*factor, factor_negated);
			const bool boolean_factor =
			    !factor_divides && IsBooleanFactor(*positive_factor) &&
			    UnwrapDecideCasts(*term, decide_index)->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE;
			if (!boolean_factor) {
				throw BinderException(*factor, "%s",
				                      DecisionScalesReducerMessage(*factor, *positive_factor, factor_divides));
			}
			out_negated = out_negated != factor_negated;
			without_negation.push_back(std::move(positive_factor));
			multipliers.push_back(without_negation.back().get());
			cur = term;
			continue;
		}
		// A row-varying factor on a reducer of a constraint is a tuple term of the row's
		// constraint: the comparison takes the VALUE shape (ClassifyScoped), which
		// multiplies the reducer's helper decision by the factor's value on each row. The
		// objective, produced once, refuses it.
		bool row_varying_tuple_term = clause == Clause::CONSTRAINT;
		if (!IsQueryWideConstant(*factor) && !row_varying_tuple_term) {
			throw BinderException(*factor, "'%s' varies per row, which cannot scale a reducer in the objective",
			                      Quote(*factor));
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
	return (func.function.name == "+" && func.children.size() == 2) || func.function.name == "-";
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
			    if (func.function.name == "*" || func.function.name == "/") {
				    const bool divides = func.function.name == "/";
				    for (idx_t side = divides ? 1 : 0; side < 2; side++) {
					    auto &candidate = *func.children[side];
					    auto &spine = *func.children[1 - side];
					    if (candidate.IsFoldable() && IsAdditiveSpine(spine, decide_index) &&
					        ReferencesDecideVar(spine)) {
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
		    bool negated;
		    auto &term = PeelScale(whole, scale, divides, negated, clause);
		    if (negated) {
			    term_sign = -term_sign;
		    }
		    if (factor) {
			    scale = ComposeScale(factor, factor_divides, std::move(scale), divides, divides);
		    }
		    // The factors peeled off a spine (`((SUM(x) - 1) / 3) / 2`, whatever its depth) distribute onto its terms
		    // as a factor written in front of one does, so the constant it holds reaches the bound side.
		    if (scale && scale->IsFoldable() && IsAdditiveSpine(term, decide_index) && ReferencesDecideVar(term)) {
			    Decompose(term, term_sign, out, clause, scale.get(), divides);
			    return;
		    }
		    if (DecomposeExtremumAddends(term_sign, term, whole, scale, divides, out)) {
			    return;
		    }
		    // Classify on the whole original term. The factors are decision-free and
		    // reducer-free by construction, so this agrees with classifying `term` -- stated
		    // rather than assumed, because Classify is the part that was wrong twice before.
		    out.push_back(Atom {term_sign, &term, Classify(whole), std::move(scale), divides, nullptr,
		                        factor || negated ? optional_idx() : whole.GetQueryLocation()});
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
	if (name != "min" && name != "max") {
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
	auto owned = RebindReducer(aggregate, name, BuildAdditive(kept));
	auto *rebuilt = owned.get();
	auto *factor = scale.get();
	out.push_back(Atom {sign, rebuilt, Classify(whole), std::move(scale), divides, std::move(owned)});
	for (auto &part : split.moved) {
		out.push_back(
		    Atom {sign * part.first, part.second, Classify(*part.second), factor ? factor->Copy() : nullptr, divides});
	}
	return true;
}

static unique_ptr<Expression> BindReducer(ClientContext &context, const string &name, unique_ptr<Expression> child) {
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

unique_ptr<Expression> DecideNormDefinition(ClientContext &context, unique_ptr<Expression> body, const string &order) {
	// Each operator by its primary name, with the name a plan prints for it (DecideRebindOperator).
	auto unary = [&](const string &name, unique_ptr<Expression> operand, const string &written_name) {
		vector<unique_ptr<Expression>> children;
		children.push_back(std::move(operand));
		return DecideRebindOperator(context, name, std::move(children), written_name);
	};
	if (order == "2") {
		vector<unique_ptr<Expression>> square;
		square.push_back(std::move(body));
		square.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(2)));
		return BindReducer(context, "sum", DecideRebindOperator(context, "**", std::move(square), "power"));
	}
	if (order == "1" || order == "inf") {
		return BindReducer(context, order == "1" ? "sum" : "max", unary("@", std::move(body), "abs"));
	}
	// An L0 norm of data counts the rows whose value is not 0; its `M` bounds a decision and means nothing here.
	// SIGN reads a NaN as 0, so a value check reads the body (DecideWrittenNormBody), not this definition.
	return BindReducer(context, "sum", unary("@", unary("sign", std::move(body), string()), "abs"));
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
		ExpressionIterator::EnumerateChildren(*expr,
		                                      [&](unique_ptr<Expression> &child) { DesugarNormsInPlace(child); });
		return;
	}
	auto &marker = expr->Cast<BoundAggregateExpression>();
	if (marker.children.size() != 1) {
		throw InternalException("DECIDE norm marker must contain one bound expression");
	}
	auto definition = DecideNormDefinition(context, marker.children[0]->Copy(), order);
	// The binder gave the marker this type (BindFunction), so what reads it reads this value in place.
	D_ASSERT(definition->return_type == marker.return_type);
	if (marker.filter) {
		definition->Cast<BoundAggregateExpression>().filter = marker.filter->Copy();
	}
	// A norm and its definition are one value (the order is in the alias, for a display).
	DecideReplaces(*definition, marker);
	auto alias = marker.GetAlias();
	RemoveDecideTag(alias, string(NORM_MARKER_TAG_PREFIX) + order + "__");
	AddDecideTag(alias, string(WRITTEN_NORM_TAG_PREFIX) + order + "__");
	definition->SetAlias(std::move(alias));
	expr = std::move(definition);
}

unique_ptr<Expression> DecideCanonicalizer::BindAggregate(const string &name, unique_ptr<Expression> child) const {
	return BindReducer(context, name, std::move(child));
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

//! `term` under its constant divisors (`e / 2 / 4`), each folded into `multiple`.
static const Expression &PeelConstantDivisors(ClientContext &context, const Expression &term, idx_t decide_index,
                                              double &multiple) {
	const Expression *node = UnwrapDecideCasts(term, decide_index);
	double divisor;
	while (node->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	       node->Cast<BoundFunctionExpression>().function.name == "/" &&
	       TryEvaluateFoldableDouble(context, *node->Cast<BoundFunctionExpression>().children[1], divisor,
	                                 FOLD_CATCH_ALL) &&
	       std::isfinite(divisor) && divisor != 0.0) {
		multiple /= divisor;
		node = UnwrapDecideCasts(*node->Cast<BoundFunctionExpression>().children[0], decide_index);
	}
	return *node;
}

//! Fold a product's sign and coefficient into `multiple`, leaving its decision-bearing factors. False when a
//! coefficient factor is data or a subquery, which is not summable.
static bool FoldProductCoefficient(ClientContext &context, DecideProductForm &form, double &multiple) {
	double coefficient = form.sign;
	for (auto &factor : form.coefficient) {
		double value;
		if (!TryEvaluateFoldableDouble(context, *factor.base, value, FOLD_CATCH_ALL) || !std::isfinite(value) ||
		    value == 0.0) {
			return false;
		}
		coefficient *= std::pow(value, static_cast<double>(factor.power));
	}
	form.coefficient.clear();
	form.sign = 1;
	multiple *= coefficient;
	return true;
}

bool DecideCanonicalizer::ReadLikeTermMultiple(const Atom &atom, DecideProductForm &term, double &multiple) const {
	multiple = atom.sign;
	if (atom.scale) {
		double scale;
		if (!TryEvaluateFoldableDouble(context, *atom.scale, scale, FOLD_PLAIN) || !std::isfinite(scale) ||
		    (atom.scale_divides && scale == 0.0)) {
			return false;
		}
		multiple = atom.scale_divides ? multiple / scale : multiple * scale;
	}
	auto &body = PeelConstantDivisors(context, *atom.expr, decide_index, multiple);
	term = ReadDecideProduct(context, body, decide_index);
	return FoldProductCoefficient(context, term, multiple);
}

void DecideCanonicalizer::ScaleAtomBy(Atom &atom, const DecideProductForm &term, double multiple) const {
	atom.sign = 1;
	if (term.factors.size() == 1 && term.factors[0].power == 1) {
		atom.expr = term.factors[0].base;
	} else {
		// The product without its constant, in the one spelling.
		auto owned = BuildDecideProductForm(context, term);
		atom.expr = owned.get();
		atom.owned = std::move(owned);
	}
	const bool whole = multiple == std::floor(multiple) && std::fabs(multiple) < 2147483647.0;
	atom.scale = make_uniq_base<Expression, BoundConstantExpression>(
	    whole ? Value::INTEGER(static_cast<int32_t>(multiple)) : Value::DOUBLE(multiple));
	atom.scale_divides = false;
}

bool DecideCanonicalizer::MergeLikeAtoms(vector<Atom> &atoms) const {
	bool merged = false;
	for (idx_t i = 0; i < atoms.size(); i++) {
		DecideProductForm term;
		double total;
		if (atoms[i].placement != Placement::LEFT || !ReadLikeTermMultiple(atoms[i], term, total)) {
			continue;
		}
		vector<idx_t> like;
		for (idx_t j = i + 1; j < atoms.size(); j++) {
			DecideProductForm other;
			double multiple;
			if (atoms[j].placement == Placement::LEFT && ReadLikeTermMultiple(atoms[j], other, multiple) &&
			    DecideFactorListsEqual(term.factors, other.factors, decide_index)) {
				total += multiple;
				like.push_back(j);
			}
		}
		// A group whose factors cancel is left as written: dropping it could empty the
		// side, and `x - x` is the user's to resolve. So is one whose sum leaves the DOUBLE range: the terms are each
		// finite and the model builder, which adds them, names the clause; a merged infinity would be a constant the
		// user never wrote.
		if (like.empty() || total == 0.0 || !std::isfinite(total)) {
			continue;
		}
		// DeciDB made the merged atom, so it has no location of its own.
		ScaleAtomBy(atoms[i], term, total);
		atoms[i].written = optional_idx();
		for (auto it = like.rbegin(); it != like.rend(); ++it) {
			atoms.erase(atoms.begin() + static_cast<int64_t>(*it));
		}
		merged = true;
	}
	return merged;
}

unique_ptr<Expression> DecideCanonicalizer::BindOp(const string &name, unique_ptr<Expression> left,
                                                   unique_ptr<Expression> right, const string &written_name) const {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(left));
	children.push_back(std::move(right));
	return DecideRebindOperator(context, name, std::move(children), written_name);
}

unique_ptr<Expression> DecideCanonicalizer::BindOp(const string &name, unique_ptr<Expression> operand,
                                                   const string &written_name) const {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(operand));
	return DecideRebindOperator(context, name, std::move(children), written_name);
}

unique_ptr<Expression> DecideCanonicalizer::BuildAdditive(const vector<Atom> &atoms) const {
	unique_ptr<Expression> acc;
	for (auto &atom : atoms) {
		int sign = atom.sign;
		auto term = atom.expr->Copy();
		if (atom.scale) {
			// Re-attach the peeled factor in the one canonical spelling: multiplication
			// with the factor on the LEFT. `SUM(x) * 2`, `2 * SUM(x)` and the cast-laden
			// variants all converge here, which is what lets each downstream consumer
			// match a single shape instead of four.
			// A data factor on a decision-bearing term that is not a scaled reducer (a merged
			// `2 * POWER(x, 2)`, a distributed `2 * (x * y)`) is a product, written as every
			// product is.
			const bool product = !atom.scale_divides && !ContainsReducer(*atom.expr) && ReferencesDecideVar(*atom.expr);
			term = atom.scale_divides ? BindOp("/", std::move(term), atom.scale->Copy())
			       : product          ? BuildDecideProduct(context, decide_index, atom.scale->Copy(), std::move(term))
			                          : BindOp("*", atom.scale->Copy(), std::move(term));
			if (atom.written.IsValid()) {
				term->SetQueryLocation(atom.written);
			}
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

bool DecideCanonicalizer::IsSignedSpine(const Expression &side, const vector<Atom> &atoms) const {
	auto root = UnwrapDecideCasts(side, decide_index);
	if (atoms.size() != 1 || root->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &name = root->Cast<BoundFunctionExpression>().function.name;
	return name == "+" || name == "-";
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeComparison(const Expression &expr) const {
	auto desugared = DesugarNorms(expr);
	auto written = NormalizeProducts(context, decide_index, desugared ? *desugared : expr);
	return CanonicalizeComparisonInternal(*written);
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeComparisonInternal(const Expression &expr) const {
	DecideProfileScope profile("canonicalize.comparison");
	auto &cmp = expr.Cast<BoundComparisonExpression>();
	// A term's sign is its own, never a factor: `-max(e)` stays a negated reducer, and `-(-max(e))` is
	// `max(e)`. The MIN/MAX rewrite routes on the tree it is given, so the tree must be the one its atoms
	// rebuild. And a tuple term may stand under a row-varying factor beside a reducer (PeelScale): the
	// VALUE shape takes it.
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
	    !IsSignedSpine(*cmp.left, left_atoms)) {
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
		auto swapped = make_uniq<BoundComparisonExpression>(FlipComparisonExpression(cmp.type), cmp.right->Copy(),
		                                                    cmp.left->Copy());
		swapped->SetAlias(cmp.GetAlias());
		DecideReplaces(*swapped, cmp);
		return CanonicalizeComparisonInternal(*swapped);
	}

	// Decision terms on BOTH sides. Migrating the right-hand ones left negates
	// them, which is only sound because the two downstream analyses consult sign, not
	// syntactic position: ABS Big-M pinning (ClassifyAbsConstraints) and a MIN/MAX's
	// push (CollectReducerSites).

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
	auto new_lhs = BuildAdditive(new_left);
	auto new_rhs = BuildAdditive(new_right);
	if (!new_rhs) {
		new_rhs = make_uniq<BoundConstantExpression>(Value::DOUBLE(0));
	}

	// K5: comparison tags are an out-of-band channel and must survive rebuilding.
	// Bound provenance is recomputed semantically from the complete canonical RHS;
	// it is never copied from whichever side happened to be the original RHS.
	auto result = make_uniq<BoundComparisonExpression>(cmp.type, std::move(new_lhs), std::move(new_rhs));
	result->SetAlias(cmp.GetAlias());
	DecideReplaces(*result, cmp);
	return FinalizeBoundProvenance(std::move(result));
}

//! Tags every reducer over decisions in `expr` that holds another reducer over decisions -- the outermost
//! of each nest -- for the value rewrite: in the objective it is a helper decision, and so
//! is every reducer inside it (D81). A reducer over data alone is a value per row: it stays in the objective's terms,
//! as a constraint's does, so a body that squares around it is the objective's own quadratic.
static void TagNestedObjectiveReducers(const DecideCanonicalizer &canonicalizer, Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &reducer = expr.Cast<BoundAggregateExpression>();
		if (!reducer.children.empty() && BoundExpressionReferencesDecide(reducer, decide_index) &&
		    canonicalizer.ContainsDecisionReducer(*reducer.children[0]) &&
		    !HasDecideTag(reducer.GetAlias(), VALUE_REWRITE_TAG)) {
			auto alias = reducer.GetAlias();
			AddDecideTag(alias, VALUE_REWRITE_TAG);
			reducer.SetAlias(std::move(alias));
		}
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    expr, [&](Expression &child) { TagNestedObjectiveReducers(canonicalizer, child, decide_index); });
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeObjective(const Expression &objective,
                                                                  double &out_constant_offset,
                                                                  DecideSense sense) const {
	auto desugared = DesugarNorms(objective);
	auto written = NormalizeProducts(context, decide_index, desugared ? *desugared : objective);
	return CanonicalizeObjectiveInternal(*written, out_constant_offset, sense == DecideSense::MAXIMIZE);
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeObjectiveInternal(const Expression &objective,
                                                                          double &out_constant_offset,
                                                                          bool negate) const {
	DecideProfileScope profile("canonicalize.objective");
	// A `when` or `per` wrapper carries no algebra: recurse into the objective child and copy
	// the condition or grouping columns unchanged. Identical rule to C0, and the same
	// wrapper predicate, so the two clauses cannot drift apart on what a wrapper is.
	if (objective.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conjunction = objective.Cast<BoundConjunctionExpression>();
		auto result = make_uniq<BoundConjunctionExpression>(conjunction.type);
		result->children.push_back(
		    CanonicalizeObjectiveInternal(*conjunction.children[0], out_constant_offset, negate));
		for (idx_t i = 1; i < conjunction.children.size(); i++) {
			result->children.push_back(conjunction.children[i]->Copy());
		}
		result->SetAlias(conjunction.GetAlias());
		// The wrapper is rebuilt around the canonical body and stands where it was written (its `per`, its `when`).
		DecideReplaces(*result, conjunction);
		return std::move(result);
	}

	// No decision content anywhere. Not a shape this pass owns, and not an error: the binder refuses it.
	if (!ReferencesDecideVar(objective)) {
		return objective.Copy();
	}

	vector<Atom> atoms;
	// The objective is canonicalized in the direction it was written: like terms merge and a factor distributes as they
	// would in a MINIMIZE. The flip of a MAXIMIZE objective is the last step (below).
	Decompose(objective, 1, atoms, Clause::OBJECTIVE);
	MergeLikeAtoms(atoms);

	vector<Atom> decision_atoms;
	for (auto &atom : atoms) {
		if (atom.placement == Placement::LEFT) {
			decision_atoms.push_back(std::move(atom));
			continue;
		}
		// A decision-free additive term shifts the objective without moving its argmax/argmin. One whose value is
		// known before the query runs is peeled here, and the body downstream is pure decision algebra. A factor
		// distributed onto it (`2 * (sum(x) - 1)`) is part of its value. Any other (a column, a data reducer, a
		// subquery) stays a term: it is one value, which the model adds to its constant, and one that differs
		// between the kept rows is a second objective (D72), which execution refuses.
		double value = 0;
		unique_ptr<Expression> scaled;
		if (atom.scale) {
			scaled = atom.scale_divides ? BindOp("/", atom.expr->Copy(), atom.scale->Copy())
			                            : BindOp("*", atom.scale->Copy(), atom.expr->Copy());
		}
		if (!TryEvaluateFoldableDouble(context, scaled ? *scaled : *atom.expr, value, FOLD_PLAIN)) {
			decision_atoms.push_back(std::move(atom));
			continue;
		}
		out_constant_offset += (negate ? -atom.sign : atom.sign) * value;
	}

	// The last step: a MAXIMIZE objective is minimized as its negation, which every term carries in its sign and the
	// offset in its value, never in a factor.
	if (negate) {
		for (auto &atom : decision_atoms) {
			atom.sign = -atom.sign;
		}
	}
	auto result = BuildAdditive(decision_atoms);
	// A reducer under a Boolean decision takes the VALUE shape, as a comparison does: the value rewrite holds the
	// reducer in a helper decision, and the product with it is a Boolean product.
	if (std::any_of(decision_atoms.begin(), decision_atoms.end(),
	                [&](const Atom &atom) { return atom.scale && ReferencesDecideVar(*atom.scale); })) {
		auto alias = result->GetAlias();
		AddDecideTag(alias, VALUE_REWRITE_TAG);
		result->SetAlias(std::move(alias));
	}
	// A reducer holding a reducer takes the value rewrite, as in a constraint: the shape is
	// decided here, once, and the rewrite reads the tag.
	TagNestedObjectiveReducers(*this, *result, decide_index);
	return result;
}

string DecideCanonicalizer::Quote(const Expression &expr, int parent_precedence) const {
	return message_names ? RenderDecideSource(expr, *message_names, parent_precedence) : UserFacingName(expr);
}

DecideCanonicalizer::ScopedShape DecideCanonicalizer::ClassifyScoped(const BoundComparisonExpression &cmp,
                                                                     idx_t data_key, idx_t &group_key) const {
	group_key = DConstants::INVALID_INDEX;
	vector<Atom> lhs_atoms;
	Decompose(*cmp.left, 1, lhs_atoms, Clause::CONSTRAINT);
	bool has_reducer = false;
	for (auto &atom : lhs_atoms) {
		has_reducer = has_reducer || ContainsDecisionReducer(*atom.expr);
	}
	if (!has_reducer) {
		// A data-only reducer sits on the right, as a bound, or inside a function of a decision (`abs(x - max(t.v))`),
		// where it is a value of its row as a column is.
		return ScopedShape::ROWS;
	}
	// The aggregate shape is one row per group: decision reducers sharing one `by` key,
	// under whole-query factors, with terms beside them that are one value per group.
	// Anything else on the decision side takes the value rewrite (VALUE): a term finer than
	// the `by` key, a factor that varies by row, a second `by` key, a reducer inside another
	// expression, a product of decisions beside a reducer.
	bool value = false;
	bool first_reducer = true;
	// The join of the scopes of the terms beside the reducers (ScopeKeyOf).
	idx_t beside_key = DConstants::INVALID_INDEX;
	// `<>` excludes an AVG's value by scaling the bound with the number of rows in the
	// group, which is exact only when every term is an AVG reading all of them.
	bool has_avg = false;
	bool only_plain_avgs = true;
	for (auto &atom : lhs_atoms) {
		if (!ContainsReducer(*atom.expr)) {
			only_plain_avgs = false;
			if (!DecideDegreeOf(context, *atom.expr, decide_index).IsLinear()) {
				value = true; // `sum(x) + b * s`: the product beside the reducer is a term of each row
			}
			beside_key = JoinDecideKeys(beside_key, ScopeKeyOf(*atom.expr, data_key));
			continue;
		}
		auto *root = UnwrapDecideCasts(*atom.expr, decide_index);
		if (root->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			only_plain_avgs = false;
			value = true; // a reducer inside another expression: `abs(sum(e) by (g) - 40)`
			continue;
		}
		auto &reducer = root->Cast<BoundAggregateExpression>();
		if (!reducer.children.empty() && ContainsReducer(*reducer.children[0])) {
			value = true; // a reducer inside it: a helper at every level (language/guide.md §5.5)
		}
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
	// A term beside the reducers is one value per group when its scope is no finer than the `by` key: a query-wide
	// one, or one whose decision and data are keyed by the `by` key itself (`per D: sum(ship) by (D) <= cap * open`
	// with `open` declared `per D:`). The group's row holds it once (CountOnce). A finer one is a term of each row.
	if (JoinDecideKeys(beside_key, group_key) != group_key) {
		value = true;
	}
	if (cmp.type == ExpressionType::COMPARE_NOTEQUAL && has_avg && !only_plain_avgs) {
		return ScopedShape::UNSUPPORTED;
	}
	return value ? ScopedShape::VALUE : ScopedShape::AGGREGATE;
}

CanonicalConstraintClass DecideCanonicalizer::ClassifyCanonicalComparison(const Expression &expr) const {
	auto &cmp = expr.Cast<BoundComparisonExpression>();
	if (ReferencesDecideVar(*cmp.right)) {
		return CanonicalConstraintClass::INVALID;
	}
	if (HasDecideTag(cmp.GetAlias(), VALUE_REWRITE_TAG)) {
		return CanonicalConstraintClass::VALUE;
	}
	return ContainsDecisionReducer(*cmp.left) ? CanonicalConstraintClass::AGGREGATE : CanonicalConstraintClass::PER_ROW;
}

void DecideCanonicalizer::ValidateCanonicalComparison(const BoundComparisonExpression &comparison,
                                                      const vector<ConstraintSourceInfo> *sources) const {
	if (ClassifyCanonicalComparison(comparison) == CanonicalConstraintClass::INVALID) {
		throw InternalException("DECIDE canonicalizer: a decision survived on the bound side of '%s'",
		                        comparison.ToString());
	}
	// A foldable bound is the one case whose NULL result is knowable at planning
	// time. Do not execute subqueries or speculate about nullable data columns here;
	// those retain the runtime COALESCE guidance.
	if (comparison.right->IsFoldable()) {
		Value value;
		if (ExpressionExecutor::TryEvaluateScalar(context, *comparison.right, value) && value.IsNull()) {
			// The bound as the user wrote it where canonicalization left it, else the clause's span.
			auto &bound = *comparison.right;
			idx_t source_id = DConstants::INVALID_INDEX;
			TryParseSourceClauseTag(comparison.GetAlias(), source_id);
			auto place = bound.GetQueryLocation().IsValid() ? bound.GetQueryLocation()
			             : sources                          ? DecideClauseLocation(*sources, source_id)
			                                                : optional_idx();
			throw BinderException(place, "the bound '%s' is NULL", Quote(bound));
		}
	}
}

void DecideCanonicalizer::ValidateCanonicalTree(const Expression &constraints,
                                                const vector<ConstraintSourceInfo> *sources) const {
	auto validate = [&](const Expression &expression) {
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			ValidateCanonicalComparison(expression.Cast<BoundComparisonExpression>(), sources);
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
		CanonicalInvariantFailure("C5", "comparison is neither a valid per-row nor aggregate constraint", comparison);
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
	if (IsWhenFilterWrapper(conjunction) && under_when) {
		CanonicalInvariantFailure("C0", "a WHEN wrapper inside another WHEN wrapper", expression);
	}
	const bool wrapper = IsConstraintWrapper(conjunction);
	const bool when = under_when || IsWhenFilterWrapper(conjunction);
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
		if (IsWhenFilterWrapper(conjunction) && conjunction.children.size() != 2) {
			CanonicalInvariantFailure("C0", "WHEN wrapper must contain one constraint and one condition", expression);
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
		// residual means an offset would be counted twice. A decision-free term that
		// does not fold (a column, a data reducer, a subquery) stays a term (D72).
		double residual_offset = 0.0;
		auto fixed_point = CanonicalizeObjective(objective, residual_offset);
		if (residual_offset != 0.0) {
			CanonicalInvariantFailure("O2", "canonical objective still yields a constant offset", objective);
		}
		if (!CanonicalTreesEqual(objective, *fixed_point)) {
			CanonicalInvariantFailure(
			    "O4/O7", "canonicalizing the objective again changes its structure, reducer scale, or tags", objective);
		}
	} catch (const InternalException &) {
		throw;
	} catch (const std::exception &ex) {
		throw InternalException("DECIDE canonical objective verification failed for %s: %s", objective.ToString(),
		                        ex.what());
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
			    "C4/C6/C7", "canonicalizing the tree again changes its structure, reducer scale, or provenance tags",
			    constraints);
		}
	} catch (const InternalException &) {
		throw;
	} catch (const std::exception &ex) {
		throw InternalException("DECIDE canonical invariant verification failed for %s: %s", constraints.ToString(),
		                        ex.what());
	}
#endif
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeTreeInternal(const Expression &constraints,
                                                                     DecideEnclosingScope scope) const {
	if (constraints.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = constraints.Cast<BoundConjunctionExpression>();
		scope = EnterDecideScope(conj, scope);

		auto result = make_uniq<BoundConjunctionExpression>(conj.type);
		result->SetAlias(conj.GetAlias());
		DecideReplaces(*result, conj); // as the objective's wrapper above
		for (idx_t i = 0; i < conj.children.size(); i++) {
			// A `when` or `per` wrapper holds the constraint in child 0; the remaining
			// children are its condition / key columns and are not constraints.
			result->children.push_back(IsConstraintChild(conj, i) ? CanonicalizeTreeInternal(*conj.children[i], scope)
			                                                      : conj.children[i]->Copy());
		}
		// `per K:` over a clause in the aggregate shape adds no row to the model: its rows
		// are one per `by` group of the kept rows, and every per-K constraint in a group
		// differs only in its bound, which the group's tightest bound covers. The wrapper
		// stays all the same: it names the classes each group's row stands for, which a
		// diagnosis reports one by one (language/guide.md §8.1, D28); the linear form reads it
		// as the clause's class key.
		return std::move(result);
	}
	if (constraints.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto result = CanonicalizeComparisonInternal(constraints);
		// The shape is decided here, once, with the clause's enclosing scope, and stamped on the comparison: a clause
		// in the aggregate shape carries the `by` key its decision reducers share, as later rewrites rebuild the
		// reducers, and one in the VALUE shape VALUE_REWRITE_TAG (ClassifyCanonicalComparison reads the stamp).
		if (!ReferencesDecideVar(*result->Cast<BoundComparisonExpression>().right)) {
			idx_t group_key;
			auto shape = ClassifyScoped(result->Cast<BoundComparisonExpression>(),
			                            scope.per_wrapper ? scope.PerKey() : DECIDE_ROW_KEY, group_key);
			if (shape == ScopedShape::UNSUPPORTED) {
				throw NotImplementedException(
				    *result, "AVG in a <> constraint beside another term, or with its own when or per, "
				             "is not supported");
			}
			idx_t stamped;
			if (shape == ScopedShape::AGGREGATE && group_key != DConstants::INVALID_INDEX &&
			    !TryParseGroupKeyTag(result->GetAlias(), stamped)) {
				auto alias = result->GetAlias();
				AddDecideTag(alias, MakeGroupKeyTag(group_key));
				result->SetAlias(std::move(alias));
			}
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

//! Whether canonicalization changed each clause: the comparison of `canonical` that carries a source clause tag against
//! the one of `written` that carries the same tag, on the bound tree. A clause the canonicalizer leaves alone comes
//! back equal.
static void ReportChangedClauses(const Expression &written, const Expression &canonical,
                                 vector<ConstraintSourceInfo> &sources) {
	unordered_map<idx_t, const BoundComparisonExpression *> before;
	ForEachConstraintLeaf(written, [&](const Expression &leaf) {
		idx_t source_id = DConstants::INVALID_INDEX;
		if (leaf.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON &&
		    TryParseSourceClauseTag(leaf.GetAlias(), source_id)) {
			before[source_id] = &leaf.Cast<BoundComparisonExpression>();
		}
	});
	ForEachConstraintLeaf(canonical, [&](const Expression &leaf) {
		idx_t source_id = DConstants::INVALID_INDEX;
		if (leaf.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON ||
		    !TryParseSourceClauseTag(leaf.GetAlias(), source_id) || source_id >= sources.size()) {
			return;
		}
		auto found = before.find(source_id);
		if (found == before.end()) {
			return;
		}
		auto &after = leaf.Cast<BoundComparisonExpression>();
		auto &was = *found->second;
		sources[source_id].canonical_differs = was.type != after.type || !Expression::Equals(*was.left, *after.left) ||
		                                       !Expression::Equals(*was.right, *after.right);
	});
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeTree(const Expression &constraints,
                                                             vector<ConstraintSourceInfo> *sources) const {
	auto desugared = DesugarNorms(constraints);
	auto written = NormalizeProducts(context, decide_index, desugared ? *desugared : constraints);
	auto result = CanonicalizeTreeInternal(*written);
	ValidateCanonicalTree(*result, sources);
	if (sources) {
		ReportChangedClauses(constraints, *result, *sources);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Variant signatures (language/guide.md §7.1)
//===--------------------------------------------------------------------===//

//! Whether two fields compare the same value: the same data, decision or `by` key.
static bool SameSignatureField(const DecideSignatureField &a, const DecideSignatureField &b) {
	if (a.kind != b.kind) {
		return false;
	}
	switch (a.kind) {
	case DecideSignatureFieldKind::VALUE:
		return a.value->Equals(*b.value);
	case DecideSignatureFieldKind::DECISION:
		return a.decision == b.decision;
	case DecideSignatureFieldKind::BY_KEY:
		return a.key == b.key;
	case DecideSignatureFieldKind::FRAME:
		return a.frame == b.frame;
	}
	return false;
}

bool DecideCanonicalizer::KeyHoldsField(idx_t key, const DecideSignatureField &field) const {
	auto holds_every_element = [&](idx_t of) {
		for (auto &element : DecideKeyElements(keys, of)) {
			if (!KeyHolds(keys, key, element.Grouping())) {
				return false;
			}
		}
		return true;
	};
	switch (field.kind) {
	case DecideSignatureFieldKind::VALUE:
		return KeyHolds(keys, key, *field.value);
	case DecideSignatureFieldKind::BY_KEY:
		// `per g: sum(x) by (g)`: the class is the group.
		return holds_every_element(field.key);
	case DecideSignatureFieldKind::DECISION: {
		if (field.decision >= variable_scopes.size()) {
			return false;
		}
		auto &scope = variable_scopes[field.decision];
		if (scope.IsGlobal()) {
			return true; // one instance for the whole query
		}
		// Keyed on elements the key holds: one instance per class already.
		return scope.IsKeyed() && scope.keyed_scope_idx < keyed_scopes.size() &&
		       holds_every_element(keyed_scopes[scope.keyed_scope_idx].key);
	}
	case DecideSignatureFieldKind::FRAME:
		return false; // a frame its key holds the timeline of is no field (CollectSignatureFields)
	}
	return false;
}

bool DecideCanonicalizer::MergesClasses(const Expression &clause, idx_t key) const {
	if (ContainsDecideFrame(clause)) {
		return false;
	}
	vector<DecideSignatureField> fields;
	CollectSignatureFields(clause, DConstants::INVALID_INDEX, fields);
	return std::none_of(fields.begin(), fields.end(),
	                    [&](const DecideSignatureField &field) { return KeyHoldsField(key, field); });
}

void DecideCanonicalizer::CollectSignatureFields(const Expression &expr, idx_t key,
                                                 vector<DecideSignatureField> &fields) const {
	auto add = [&](DecideSignatureField field) {
		if (KeyHoldsField(key, field)) {
			return;
		}
		for (auto &existing : fields) {
			if (SameSignatureField(existing, field)) {
				return;
			}
		}
		fields.push_back(std::move(field));
	};
	idx_t frame;
	if (TryGetDecideFrame(expr, frame) && frames && frame < frames->size()) {
		// A frame is compared by its keys, as the annotated tree holds it (D82, deck p. 99): its timeline, the anchor's
		// place on it when its position is relative to the anchor's, and its fallback where the anchor reaches it.
		// `first` and `last` read one value per timeline, so a key that holds the timeline's splits nothing further.
		auto &spec = (*frames)[frame];
		if (!spec.Relative()) {
			bool covered = true;
			for (auto &element : DecideKeyElements(keys, spec.within_key)) {
				covered = covered && KeyHolds(keys, key, element.Grouping());
			}
			if (covered) {
				return;
			}
		}
		DecideSignatureField field;
		field.kind = DecideSignatureFieldKind::FRAME;
		field.frame = frame;
		field.text = spec.text;
		add(std::move(field));
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		// A reducer reads a group, not the row: its `by` key is the field, and its body is a scoped expression of its
		// own.
		idx_t by;
		if (TryParseGroupKeyTag(expr.GetAlias(), by) && by < keys.size()) {
			DecideSignatureField field;
			field.kind = DecideSignatureFieldKind::BY_KEY;
			field.key = by;
			field.text = "by (" + DecideKeyText(keys, by) + ")";
			add(std::move(field));
		}
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	    expr.Cast<BoundColumnRefExpression>().binding.table_index == decide_index) {
		// A decision is compared by instance.
		DecideSignatureField field;
		field.kind = DecideSignatureFieldKind::DECISION;
		field.decision = expr.Cast<BoundColumnRefExpression>().binding.column_index;
		field.text = StripDecideTags(expr.GetName());
		add(std::move(field));
		return;
	}
	if (!ReferencesDecideVar(expr) && !ContainsReducer(expr) && !ContainsDecideFrame(expr)) {
		// Data is compared leaf by leaf (deck pp. 81, 110): each column it reads is a field, so two rows whose terms
		// happen to sum alike are two variants. A literal or a query-wide value has one value everywhere, and data that
		// reads no column (`random()`) is a field whole.
		if (expr.IsFoldable() || IsQueryWideConstant(expr)) {
			return;
		}
		bool leaves = false;
		std::function<void(const Expression &)> read_leaves = [&](const Expression &part) {
			if (part.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				leaves = true;
				DecideSignatureField field;
				field.value = part.Copy();
				field.text = Quote(part);
				add(std::move(field));
				return;
			}
			ExpressionIterator::EnumerateChildren(part, read_leaves);
		};
		read_leaves(expr);
		if (!leaves) {
			DecideSignatureField field;
			field.value = expr.Copy();
			field.text = Quote(expr);
			add(std::move(field));
		}
		return;
	}
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](const Expression &child) { CollectSignatureFields(child, key, fields); });
}

//! Appends a signature to `out` and returns its index.
static idx_t AddSignature(DecideSignatures &out, const Expression *rows, idx_t key, vector<DecideSignatureField> fields,
                          string text, idx_t source_clause_id) {
	DecideSignature signature;
	signature.rows = rows ? rows->Copy() : nullptr;
	signature.key = key;
	signature.fields = std::move(fields);
	signature.text = std::move(text);
	signature.source_clause_id = source_clause_id;
	out.signatures.push_back(std::move(signature));
	return out.signatures.size() - 1;
}

void DecideCanonicalizer::SignQualifiedReducers(Expression &expr, const DecideReducerPath &path, Clause clause,
                                                idx_t source_clause_id, DecideSignatures &out) const {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		ExpressionIterator::EnumerateChildren(
		    expr, [&](Expression &child) { SignQualifiedReducers(child, path, clause, source_clause_id, out); });
		return;
	}
	auto &reducer = expr.Cast<BoundAggregateExpression>();
	// The reducers in its argument first, along its path (language/guide.md §5.5).
	auto below = path.Below(reducer);
	for (auto &child : reducer.children) {
		SignQualifiedReducers(*child, below, clause, source_clause_id, out);
	}
	idx_t scope_idx;
	// A reducer over data reads one value per class and value of its body: its table groups on both
	// (DirectAddBoundTables), so it needs no scope of its own.
	if (!TryParseQualifiedReducerTag(reducer.GetAlias(), scope_idx) || scope_idx >= out.keyed_scopes.size() ||
	    reducer.children.empty() || !ReferencesDecideVar(reducer)) {
		return;
	}
	// The reducer's own `per K'` -- or `D` of the colon form `SUM(D: e)` -- hands it one value per class and signature
	// of its body (D70, D71), over the rows its `when` keeps.
	auto key = out.keyed_scopes[scope_idx].key;
	vector<DecideSignatureField> fields;
	CollectSignatureFields(*reducer.children[0], key, fields);
	if (fields.empty()) {
		return; // the key's classes are its instances already
	}
	// The rows it reads: its path's, then its own `when`'s.
	auto reducer_rows = below.Rows();
	auto text = message_names ? RenderDecideSource(reducer, *message_names) : reducer.ToString();
	auto signature = AddSignature(out, reducer_rows.get(), key, std::move(fields), std::move(text), source_clause_id);
	// A scope of its own, on the same key: the classes it reads are the key's split by the signature, which no other
	// reducer's or declaration's sharing the key are.
	KeyedScopeInfo variant_scope;
	variant_scope.key = key;
	variant_scope.signature = signature;
	out.keyed_scopes.push_back(std::move(variant_scope));
	auto alias = reducer.GetAlias();
	RemoveDecideTag(alias, MakeQualifiedReducerTag(scope_idx));
	AddDecideTag(alias, MakeQualifiedReducerTag(out.keyed_scopes.size() - 1));
	reducer.SetAlias(std::move(alias));
}

void DecideCanonicalizer::BuildConstraintSignatures(Expression &constraints, DecideSignatures &out) const {
	BuildConstraintSignatures(constraints, nullptr, out);
}

void DecideCanonicalizer::BuildConstraintSignatures(Expression &expr, const Expression *when,
                                                    DecideSignatures &out) const {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		idx_t source_clause_id = DConstants::INVALID_INDEX;
		TryParseSourceClauseTag(expr.GetAlias(), source_clause_id);
		DecideReducerPath path;
		if (when) {
			path.conditions.push_back(when);
		}
		SignQualifiedReducers(expr, path, Clause::CONSTRAINT, source_clause_id, out);
		return;
	}
	auto &conj = expr.Cast<BoundConjunctionExpression>();
	auto scope = EnterDecideScope(conj, DecideEnclosingScope());
	const Expression *inner_when = scope.when_wrapper ? scope.Condition() : when;
	for (idx_t i = 0; i < conj.children.size(); i++) {
		if (IsConstraintChild(conj, i)) {
			BuildConstraintSignatures(*conj.children[i], inner_when, out);
		}
	}
	if (!scope.per_wrapper) {
		return;
	}
	// `per K: C` is one instance per class of K and distinct value of C's fields (D70): the columns its tuple terms
	// read and its decisions outside reducers, and its reducers' `by` keys (D83). A `per` wrapper holds one
	// clause (C0), under at most one `when`.
	const Expression *rows = when;
	idx_t source_clause_id = DConstants::INVALID_INDEX;
	vector<DecideSignatureField> fields;
	auto key = scope.PerKey();
	ForEachScopedConstraintLeaf(*conj.children[0], [&](const Expression &leaf, const DecideEnclosingScope &below) {
		if (below.when_wrapper) {
			rows = below.Condition();
		}
		CollectSignatureFields(leaf, key, fields);
		if (source_clause_id == DConstants::INVALID_INDEX) {
			TryParseSourceClauseTag(leaf.GetAlias(), source_clause_id);
		}
	});
	if (fields.empty()) {
		return; // one instance per class, as the key alone gives
	}
	auto key_text = message_names ? RenderDecidePerKey(conj, message_names->text, &keys) : DecideKeyText(keys, key);
	auto signature = AddSignature(out, rows, key, std::move(fields), "per " + key_text, source_clause_id);
	auto alias = conj.GetAlias();
	AddDecideTag(alias, MakeSignatureTag(signature));
	conj.SetAlias(std::move(alias));
}

void DecideCanonicalizer::BuildObjectiveSignatures(Expression &objective, DecideSignatures &out) const {
	Expression *body = &objective;
	const Expression *when = nullptr;
	if (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = body->Cast<BoundConjunctionExpression>();
		if (IsWhenFilterWrapper(conj) && conj.children.size() == 2) {
			when = conj.children[1].get();
			body = conj.children[0].get();
		}
	}
	DecideReducerPath path;
	if (when) {
		path.conditions.push_back(when);
	}
	SignQualifiedReducers(*body, path, Clause::OBJECTIVE, DConstants::INVALID_INDEX, out);
	// Produced once (`per ()`, D72): a field with a second value over the kept rows would be a second objective, which
	// execution refuses.
	vector<DecideSignatureField> fields;
	CollectSignatureFields(*body, DConstants::INVALID_INDEX, fields);
	if (!fields.empty()) {
		out.objective = AddSignature(out, when, DConstants::INVALID_INDEX, std::move(fields), "the objective",
		                             DConstants::INVALID_INDEX);
	}
}

//===--------------------------------------------------------------------===//
// Frame placement (language/guide.md §5.6)
//===--------------------------------------------------------------------===//

bool DecideCanonicalizer::ReducerPerKey(const BoundAggregateExpression &reducer,
                                        const vector<KeyedScopeInfo> &keyed_scopes, FramePerKey &out) const {
	idx_t scope_idx;
	if (!TryParseQualifiedReducerTag(reducer.GetAlias(), scope_idx) || scope_idx >= keyed_scopes.size()) {
		return false;
	}
	out.key = keyed_scopes[scope_idx].key;
	out.text = "per " + DecideKeyText(keys, out.key);
	return true;
}

void DecideCanonicalizer::PlaceFramesIn(const Expression &expr, const DecideReducerPath &path, const FramePerKey *per,
                                        DecideFramePlacement &placement) const {
	idx_t frame_idx;
	if (TryGetDecideFrame(expr, frame_idx)) {
		if (frame_idx >= placement.frames.size()) {
			throw InternalException("DECIDE: a frame marker names frame %llu of %llu", frame_idx,
			                        placement.frames.size());
		}
		if (placement.placed[frame_idx]) {
			throw InternalException("DECIDE: frame %llu stands in two places", frame_idx);
		}
		placement.placed[frame_idx] = true;
		auto &frame = placement.frames[frame_idx];
		frame.rows = path.Rows();
		// Its timelines are split by the path's `by` keys, then by `within`.
		frame.timeline_key = FindOrCreateDecidePathKey(path.KeysOf(frame.within_key), placement.keys);
		frame.timeline_names.clear();
		for (auto &element : DecideKeyElements(placement.keys, frame.timeline_key)) {
			frame.timeline_names.push_back(element.name);
		}
		if (!per) {
			return;
		}
		// D75: each class of the `per` key reads one timeline. Each is inside one group of the path's keys, so only
		// `within` can split one -- unless the key, with the path's, holds it.
		auto class_key = FindOrCreateDecidePathKey(path.KeysOf(per->key), placement.keys);
		bool spans = false;
		for (auto &element : DecideKeyElements(placement.keys, frame.within_key)) {
			spans = spans || !KeyHolds(placement.keys, class_key, element.Grouping());
		}
		if (!spans) {
			return;
		}
		frame.partition_key = class_key;
		frame.partition_subject =
		    per->text + " does not determine the timeline " + StringUtil::Join(frame.timeline_names, ", ");
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		// A reducer's argument is a scoped expression of its own: its frames read the rows of the reducer's members,
		// along its path, under its own `per`.
		auto &inner = expr.Cast<BoundAggregateExpression>();
		auto below = path.Below(inner);
		FramePerKey own;
		bool has_per = ReducerPerKey(inner, placement.keyed_scopes, own);
		// Its value is one place: a collated MIN or MAX reads it again as the key it compares (BindMinMax).
		if (!inner.children.empty()) {
			PlaceFramesIn(*inner.children[0], below, has_per ? &own : nullptr, placement);
		}
		return;
	}
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](const Expression &child) { PlaceFramesIn(child, path, per, placement); });
}

void DecideCanonicalizer::PlaceClauseFrames(const Expression &expr, DecideEnclosingScope enclosing,
                                            DecideFramePlacement &placement) const {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		auto inner = EnterDecideScope(conj, enclosing);
		for (idx_t i = 0; i < conj.children.size(); i++) {
			if (IsConstraintChild(conj, i)) {
				PlaceClauseFrames(*conj.children[i], inner, placement);
			}
		}
		return;
	}
	DecideReducerPath path;
	if (enclosing.Condition()) {
		path.conditions.push_back(enclosing.Condition());
	}
	FramePerKey per;
	if (enclosing.per_wrapper) {
		per.key = enclosing.PerKey();
		// The key as written (`per D.depotID`), cut from the query.
		per.text = "per " + (message_names ? RenderDecidePerKey(*enclosing.per_wrapper, message_names->text, &keys)
		                                   : DecideKeyText(keys, per.key));
	}
	PlaceFramesIn(expr, path, enclosing.per_wrapper ? &per : nullptr, placement);
}

void DecideCanonicalizer::PlaceFrames(const Expression &constraints, DecideFramePlacement &placement) const {
	placement.placed.resize(placement.frames.size(), false);
	PlaceClauseFrames(constraints, DecideEnclosingScope(), placement);
}

void DecideCanonicalizer::PlaceObjectiveFrames(const Expression &objective, DecideFramePlacement &placement) const {
	placement.placed.resize(placement.frames.size(), false);
	const Expression *body = &objective;
	DecideReducerPath path;
	if (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = body->Cast<BoundConjunctionExpression>();
		if (IsWhenFilterWrapper(conj) && conj.children.size() == 2) {
			path.conditions.push_back(conj.children[1].get());
			body = conj.children[0].get();
		}
	}
	// The objective is produced once, `per ()` (deck p. 62), so its one class lies in one timeline of every frame at
	// its root, as a `per ()` clause's does (D75).
	FramePerKey per;
	per.text = "the objective (per ())";
	PlaceFramesIn(*body, path, &per, placement);
}

} // namespace duckdb
