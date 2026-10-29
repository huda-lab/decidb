#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/table_binding.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include <unordered_set>
#include <algorithm>
#include <cmath>

#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"

namespace duckdb {

bool IsVariableExpression(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	const auto &colref = expr.Cast<const ColumnRefExpression>();
	// A decision is referenced by its bare name; the declarations binder refuses `D.x`.
	return !colref.IsQualified() && variables.count(colref.GetColumnName()) > 0;
}

//! ParsedExpressionIterator::EnumerateChildren, plus the body (SELECT list, WHERE,
//! FROM, ...) of a subquery, which EnumerateChildren does not enter. A DECIDE nested in
//! the subquery keeps its clauses to itself: its own binder checks them.
static void EnumerateChildrenAndSubqueries(const ParsedExpression &expr,
                                           const std::function<void(const ParsedExpression &child)> &callback) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery_expr = expr.Cast<const SubqueryExpression>();
		if (subquery_expr.subquery && subquery_expr.subquery->node) {
			// const_cast: EnumerateQueryNodeChildren requires non-const but we only read
			ParsedExpressionIterator::EnumerateQueryNodeChildren(
			    const_cast<QueryNode &>(*subquery_expr.subquery->node),
			    [&](unique_ptr<ParsedExpression> &child) { callback(*child); }, [&](TableRef &ref) {},
			    /*decide_clauses=*/false);
		}
	}
	ParsedExpressionIterator::EnumerateChildren(expr, callback);
}

static idx_t CountDecideVariableOccurrencesInternal(const ParsedExpression &expr,
                                                    const case_insensitive_map_t<idx_t> &variables) {
	idx_t count = 0;
	if (IsVariableExpression(expr, variables)) {
		count++;
	}
	EnumerateChildrenAndSubqueries(expr, [&](const ParsedExpression &child) {
		count += CountDecideVariableOccurrencesInternal(child, variables);
	});
	return count;
}

bool ExpressionContainsDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	return CountDecideVariableOccurrencesInternal(expr, variables) > 0;
}

static bool IsPowerFunction(const FunctionExpression &func);

void ValidateDecideNoExplicitDecisionCasts(const ParsedExpression &expr,
                                           const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() == ExpressionClass::CAST) {
		auto &cast = expr.Cast<const CastExpression>();
		// Transformer::TransformTypeCast stamps CAST, TRY_CAST and :: with their
		// source location. CastExpression nodes synthesized by the parser itself
		// (for example for a boolean test) have no location and are not user casts.
		if (cast.GetQueryLocation().IsValid() && ExpressionContainsDecideVariable(*cast.child, variables)) {
			throw BinderException(
			    cast,
			    "DECIDE does not allow casts over decision expressions: '%s'. "
			    "Decision values are modeled directly as DOUBLE. Remove the cast; "
			    "casts over data-only expressions are allowed.",
			    cast.ToString());
		}
	}

	// Subquery bodies too, so a correlated decision reference cannot hide an
	// explicit cast inside one.
	EnumerateChildrenAndSubqueries(
	    expr, [&](const ParsedExpression &child) { ValidateDecideNoExplicitDecisionCasts(child, variables); });
}

//===--------------------------------------------------------------------===//
// Where when, per and by may stand
//===--------------------------------------------------------------------===//

//! Refuses every `when`, `per` and `by` inside `expr`, subquery bodies included, as not bound
//! there: a `when` condition, a key, a `by` key and an ordinary SQL subquery written in
//! the clause (which is lexed with the clause's keywords) take data only.
static void ValidateNoScopeSpelling(const ParsedExpression &expr) {
	static const struct {
		const char *tag;
		const char *spelling;
	} scopes[] = {{SCOPE_WHEN_TAG, "WHEN condition:"},
	              {SCOPE_PER_TAG, "PER key:"},
	              {SCOPE_PER_ROW_TAG, "PER ROW:"},
	              {REDUCER_BY_TAG, "BY (key)"}};
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		for (auto &scope : scopes) {
			if (func.is_operator && func.function_name == scope.tag) {
				bool global = func.function_name == SCOPE_PER_TAG && func.children.size() == 1;
				throw BinderException(expr, "DECIDE: '%s' is not supported yet", global ? "PER ():" : scope.spelling);
			}
		}
		if (IsDecideMarker(expr, QUALIFIED_REDUCER_TAG)) {
			throw BinderException(expr, "DECIDE: the colon form %s is not supported inside a subquery yet",
			                      expr.ToString());
		}
	}
	EnumerateChildrenAndSubqueries(expr, [&](const ParsedExpression &child) { ValidateNoScopeSpelling(child); });
}

//! The first decision `expr` names, as written, or empty: `when`, `per` and `by` take data.
static string FindDecisionName(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	if (IsVariableExpression(expr, variables)) {
		return expr.ToString();
	}
	string found;
	EnumerateChildrenAndSubqueries(expr, [&](const ParsedExpression &child) {
		if (found.empty()) {
			found = FindDecisionName(child, variables);
		}
	});
	return found;
}

//! The first node of `expr`, outside its subqueries, that `matches`, or null.
static optional_ptr<const ParsedExpression> FindNode(const ParsedExpression &expr,
                                                     const std::function<bool(const ParsedExpression &)> &matches) {
	if (matches(expr)) {
		return &expr;
	}
	optional_ptr<const ParsedExpression> found;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (!found) {
			found = FindNode(child, matches);
		}
	});
	return found;
}

//! A `when` condition, a key element or a `by` key: data only, no `when`/`per`/`by`, and
//! no window function or reducer, which read rows the scope has not chosen yet.
static void ValidateScopeData(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	auto decision = FindDecisionName(expr, variables);
	if (!decision.empty()) {
		throw BinderException(expr, "DECIDE: %s is a decision; when, per and by may only use data", decision);
	}
	auto window = FindNode(
	    expr, [](const ParsedExpression &node) { return node.GetExpressionClass() == ExpressionClass::WINDOW; });
	if (window) {
		throw BinderException(*window, "DECIDE: a window function such as '%s' is not supported in when, per or by",
		                      window->ToString());
	}
	auto reducer = FindNode(expr, [](const ParsedExpression &node) { return TakesReducerBy(node); });
	if (reducer) {
		throw BinderException(*reducer, "DECIDE: a reducer such as '%s' is not supported in when, per or by",
		                      reducer->ToString());
	}
	ValidateNoScopeSpelling(expr);
}

//! `[when c] [per K]:` off a clause or a reducer's argument: the key elements and the
//! condition are validated as data, and the body is returned.
static const ParsedExpression &PeelScopeSpelling(const ParsedExpression &expr,
                                                 const case_insensitive_map_t<idx_t> &variables) {
	const ParsedExpression *body = &expr;
	if (IsDecideMarker(*body, SCOPE_PER_ROW_TAG)) {
		body = body->Cast<const FunctionExpression>().children[0].get();
	} else if (IsDecideMarker(*body, SCOPE_PER_TAG)) {
		auto &per = body->Cast<const FunctionExpression>();
		for (idx_t i = 1; i < per.children.size(); i++) {
			ValidateScopeData(*per.children[i], variables);
		}
		body = per.children[0].get();
	}
	if (IsDecideMarker(*body, SCOPE_WHEN_TAG)) {
		auto &when = body->Cast<const FunctionExpression>();
		ValidateScopeData(*when.children[1], variables);
		body = when.children[0].get();
	}
	return *body;
}

namespace {
//! Where in a clause a node stands, for the placement rules.
struct ScopePlacement {
	const case_insensitive_map_t<idx_t> &variables;
	//! Inside a reducer's argument.
	bool in_reducer = false;
	//! The objective's two-level form: its one inner reducer is allowed.
	bool two_level = false;
	//! Under a `*` or `/` that holds a decision: a reducer over data alone there scales the
	//! decision, a coefficient ValidateDecideNoNonLinearScalar refuses with its remedy.
	bool scales_a_decision = false;
};
} // namespace

static void ValidateScopePlacement(const ParsedExpression &expr, ScopePlacement placement) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		ValidateNoScopeSpelling(expr);
		return;
	}
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		ParsedExpressionIterator::EnumerateChildren(
		    expr, [&](const ParsedExpression &child) { ValidateScopePlacement(child, placement); });
		return;
	}
	auto &func = expr.Cast<const FunctionExpression>();
	if (func.is_operator && func.function_name == REDUCER_BY_TAG) {
		if (!TakesReducerBy(UnwrapReducerMarkers(*func.children[0]))) {
			throw BinderException(expr, "DECIDE: by (...) must directly follow SUM, AVG, MIN, MAX, norm or count");
		}
		for (idx_t i = 1; i < func.children.size(); i++) {
			ValidateScopeData(*func.children[i], placement.variables);
		}
		ValidateScopePlacement(*func.children[0], placement);
		return;
	}
	if (IsDecideScopeMarker(expr)) {
		// Only in front of a clause or of a reducer's argument, both peeled before here.
		ValidateNoScopeSpelling(expr);
	}
	bool scoped_argument = !func.is_operator && !func.children.empty() && IsDecideScopeMarker(*func.children[0]);
	if (scoped_argument && !IsDecideAggregateName(func.function_name)) {
		throw BinderException(expr, "DECIDE: when and per inside a call need SUM, AVG, MIN or MAX; write the scope "
		                            "in front of the constraint or inside a reducer");
	}
	bool reducer = !func.is_operator &&
	               (IsDecideAggregateName(func.function_name) || StringUtil::CIEquals(func.function_name, "norm"));
	if (reducer && placement.in_reducer && !placement.two_level &&
	    !(placement.scales_a_decision && !ExpressionContainsDecideVariable(expr, placement.variables))) {
		throw BinderException(expr, "DECIDE: an aggregate inside an aggregate is not supported yet");
	}
	if (func.is_operator && (func.function_name == "*" || func.function_name == "/") &&
	    ExpressionContainsDecideVariable(expr, placement.variables)) {
		placement.scales_a_decision = true;
	}
	auto inner = placement;
	if (reducer) {
		// The two-level objective allows the outer reducer's one inner reducer, no deeper.
		inner.two_level = placement.two_level && !placement.in_reducer;
		inner.in_reducer = true;
	}
	for (idx_t i = 0; i < func.children.size(); i++) {
		auto &child =
		    i == 0 && scoped_argument ? PeelScopeSpelling(*func.children[0], placement.variables) : *func.children[i];
		ValidateScopePlacement(child, reducer ? inner : placement);
	}
	if (func.filter) {
		ValidateNoScopeSpelling(*func.filter);
	}
}

bool IsTwoLevelObjective(const ParsedExpression &body) {
	const ParsedExpression *outer = &body;
	if (IsDecideMarker(*outer, REDUCER_BY_TAG)) {
		auto &by = outer->Cast<const FunctionExpression>();
		if (by.children.size() != 1) {
			return false;
		}
		outer = by.children[0].get();
	}
	if (!IsScopedReducerCall(*outer)) {
		return false;
	}
	auto &outer_call = outer->Cast<const FunctionExpression>();
	if (outer_call.children.size() != 1 || !IsDecideMarker(*outer_call.children[0], SCOPE_PER_TAG)) {
		return false;
	}
	auto &per = outer_call.children[0]->Cast<const FunctionExpression>();
	if (per.children.size() < 2) {
		return false;
	}
	// `outer(when c per k: inner(e) by (k))` is the same form with a `when` the binder
	// refuses by name.
	auto *inner = per.children[0].get();
	if (IsDecideMarker(*inner, SCOPE_WHEN_TAG)) {
		inner = inner->Cast<const FunctionExpression>().children[0].get();
	}
	return IsDecideMarker(*inner, REDUCER_BY_TAG) &&
	       IsScopedReducerCall(*inner->Cast<const FunctionExpression>().children[0]);
}

static void ValidateScopeClause(const ParsedExpression &clause, const case_insensitive_map_t<idx_t> &variables,
                                bool objective) {
	auto &body = PeelScopeSpelling(clause, variables);
	// Every clause follows the scope rules (D30).
	ScopePlacement placement {variables};
	placement.two_level = objective && IsTwoLevelObjective(body);
	ValidateScopePlacement(body, placement);
}

void ValidateDecideScopeSpellings(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                  bool objective) {
	if (!objective && expr.GetExpressionClass() == ExpressionClass::CONJUNCTION &&
	    expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		// `AND` separates constraints; each is checked on its own.
		for (auto &child : expr.Cast<const ConjunctionExpression>().children) {
			ValidateDecideScopeSpellings(*child, variables, objective);
		}
		return;
	}
	ValidateScopeClause(expr, variables, objective);
}

//! Column `column_index` of `binding` as a key column. A base table's goes through
//! GetColumnBinding, which also forces the column into the scan and returns its
//! position there; any other relation's binding is (table index, column).
static ColumnBinding RegisterKeyColumn(Binding &binding, column_t column_index) {
	if (binding.binding_type == BindingType::TABLE) {
		return binding.Cast<TableBinding>().GetColumnBinding(column_index);
	}
	return ColumnBinding(binding.index, column_index);
}

//! Whether column `column_index` of `binding` stores a value per row: false for `rowid`
//! and for a generated column, which a key would only repeat.
static bool IsStoredColumn(Binding &binding, column_t column_index) {
	if (column_index == COLUMN_IDENTIFIER_ROW_ID) {
		return false;
	}
	auto entry = binding.GetStandardEntry();
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return true;
	}
	return !entry->Cast<TableCatalogEntry>().GetColumn(LogicalIndex(column_index)).Generated();
}

//! One column of a key: where it binds, its type, its name as a message quotes it -- as
//! written for a column element, qualified by its relation (`D.depotID`) for a column a
//! relation element stands for -- and how it compares NULL, which follows the same split.
struct KeyColumn {
	ColumnBinding binding;
	LogicalType type;
	string name;
	DecideKeyNullMode null_mode;
};
using KeyColumns = vector<KeyColumn>;

//! Column `column_index` of `binding` as a key column a relation element stands for,
//! registered with the scan.
static KeyColumn MakeKeyColumn(Binding &binding, column_t column_index) {
	return KeyColumn {RegisterKeyColumn(binding, column_index), binding.types[column_index],
	                  KeywordHelper::WriteOptionallyQuoted(binding.GetAlias()) + "." +
	                      KeywordHelper::WriteOptionallyQuoted(binding.names[column_index]),
	                  DecideKeyNullMode::VALUE};
}

//! Adds the stored columns of `relation`: a relation in a key stands for them.
static void AddRelationColumns(Binding &relation, KeyColumns &columns) {
	for (column_t column_index = 0; column_index < relation.names.size(); column_index++) {
		if (IsStoredColumn(relation, column_index)) {
			columns.push_back(MakeKeyColumn(relation, column_index));
		}
	}
}

//! Adds column `column_index` of `binding`, which `element` names, to `columns`.
static void AddKeyColumn(const ColumnRefExpression &element, Binding &binding, column_t column_index,
                         KeyColumns &columns) {
	if (!IsStoredColumn(binding, column_index)) {
		throw BinderException(element,
		                      "DECIDE: %s is not a stored column; a per key lists columns or relations of the "
		                      "FROM clause",
		                      element.ToString());
	}
	auto column = MakeKeyColumn(binding, column_index);
	column.name = element.ToString();
	column.null_mode = DecideKeyNullMode::NO_GROUP;
	columns.push_back(std::move(column));
}

//! The relation named by up to three parts: `alias`, `schema.alias`, `catalog.schema.alias`.
static BindingAlias RelationAlias(const vector<string> &parts) {
	switch (parts.size()) {
	case 1:
		return BindingAlias(parts[0]);
	case 2:
		return BindingAlias(parts[0], parts[1]);
	default:
		D_ASSERT(parts.size() == 3);
		return BindingAlias(parts[0], parts[1], parts[2]);
	}
}

//! The column `name` written unqualified, as it binds anywhere else in the query: a USING
//! column means its primary relation's, and after a FULL OUTER JOIN, which has none, the
//! key must say which side it means. Null when no relation has the column.
static optional_ptr<Binding> UnqualifiedColumnBinding(BindContext &bind_context, const ColumnRefExpression &element) {
	auto &name = element.GetColumnName();
	auto using_set = bind_context.GetUsingBinding(name);
	if (!using_set) {
		return bind_context.GetMatchingBinding(name);
	}
	if (!using_set->primary_binding.IsSet()) {
		vector<string> choices;
		for (auto &side : using_set->bindings) {
			choices.push_back(KeywordHelper::WriteOptionallyQuoted(side.GetAlias()) + "." +
			                  KeywordHelper::WriteOptionallyQuoted(name));
		}
		throw BinderException(element, "DECIDE: %s is merged by a FULL OUTER JOIN USING; write %s in the per key",
		                      element.ToString(), StringUtil::Join(choices, " or "));
	}
	ErrorData error;
	return bind_context.GetBinding(using_set->primary_binding, error);
}

//! Resolves one element of a `per` key into `columns`, against the FROM clause alone and
//! the way SQL resolves the same name: a column (`c`, `R.c`, `S.R.c`, `C.S.R.c`), else a
//! relation (`R`, `S.R`, `C.S.R`), standing for every column it stores.
static void ResolveKeyElement(BindContext &bind_context, const ColumnRefExpression &element,
                              const case_insensitive_set_t &decision_names, KeyColumns &columns) {
	auto &names = element.column_names;
	if (names.size() == 1 && decision_names.count(names[0])) {
		throw BinderException(element, "DECIDE: %s is a decision; when, per and by may only use data",
		                      element.ToString());
	}
	ErrorData error;
	optional_ptr<Binding> binding;
	if (names.size() == 1) {
		binding = UnqualifiedColumnBinding(bind_context, element);
	} else if (names.size() <= 4) {
		binding =
		    bind_context.GetBinding(RelationAlias(vector<string>(names.begin(), names.end() - 1)), names.back(), error);
	}
	if (binding) {
		AddKeyColumn(element, *binding, binding->GetBindingIndex(names.back()), columns);
		return;
	}
	auto relation = names.size() <= 3 ? bind_context.GetBinding(RelationAlias(names), error) : nullptr;
	if (!relation) {
		throw BinderException(element, "DECIDE: %s is neither a column nor a relation of the FROM clause",
		                      element.ToString());
	}
	AddRelationColumns(*relation, columns);
}

//! The columns of the key `per` lists (its children[1..]; none for `per ()`), each element
//! resolved by ResolveKeyElement.
static KeyColumns ResolveKey(BindContext &bind_context, const FunctionExpression &per,
                             const case_insensitive_set_t &decision_names) {
	KeyColumns columns;
	for (idx_t i = 1; i < per.children.size(); i++) {
		auto &element = *per.children[i];
		if (element.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			throw InternalException("DECIDE per key element '%s' is not a name", element.ToString());
		}
		ResolveKeyElement(bind_context, element.Cast<ColumnRefExpression>(), decision_names, columns);
	}
	if (columns.empty() && per.children.size() > 1) {
		throw InternalException("DECIDE per key '%s' resolved to no column", DecideKeyToString(per));
	}
	return columns;
}

//! `columns` as key elements, for FindOrCreateDecideKey to order and de-duplicate.
static vector<DecideKeyElement> KeyElements(const KeyColumns &columns) {
	vector<DecideKeyElement> elements;
	for (auto &column : columns) {
		DecideKeyElement element;
		element.expression = make_uniq<BoundColumnRefExpression>(column.name, column.type, column.binding);
		element.name = column.name;
		element.null_mode = column.null_mode;
		elements.push_back(std::move(element));
	}
	return elements;
}

//! The scope of the decisions and reducers keyed by `key`: one per key.
static idx_t FindOrCreateScope(idx_t key, vector<EntityScopeInfo> &entity_scopes) {
	for (idx_t scope_idx = 0; scope_idx < entity_scopes.size(); scope_idx++) {
		if (entity_scopes[scope_idx].key == key && entity_scopes[scope_idx].value_link == DConstants::INVALID_INDEX) {
			return scope_idx;
		}
	}
	EntityScopeInfo scope_info;
	scope_info.key = key;
	entity_scopes.push_back(std::move(scope_info));
	return entity_scopes.size() - 1;
}

idx_t FindOrCreateKeyScope(BindContext &bind_context, const FunctionExpression &per,
                           const case_insensitive_set_t &decision_names, const string &key_text,
                           vector<DecideKey> &keys, vector<EntityScopeInfo> &entity_scopes) {
	auto key = BindKey(bind_context, per, decision_names, key_text, keys);
	return FindOrCreateScope(key, entity_scopes);
}

idx_t BindKey(BindContext &bind_context, const FunctionExpression &per, const case_insensitive_set_t &decision_names,
              const string &key_text, vector<DecideKey> &keys) {
	return FindOrCreateDecideKey(KeyElements(ResolveKey(bind_context, per, decision_names)), key_text, keys);
}

//! The relation `element` names when it names no column of the FROM clause: `by (D)`
//! stands for D's stored columns, as `per D` does. Null for a column or an unknown name.
static optional_ptr<Binding> ResolveRelationName(BindContext &bind_context, const ColumnRefExpression &element) {
	auto &names = element.column_names;
	ErrorData error;
	// A column wins, as it does in SQL; an ambiguous one is still a column, left for the
	// ordinary binder to report.
	bool column = false;
	if (names.size() == 1) {
		column = bind_context.GetUsingBinding(names[0]) != nullptr;
		for (auto &binding : bind_context.GetBindingsList()) {
			column = column || binding->HasMatchingBinding(names[0]);
		}
	} else if (names.size() <= 4) {
		column = bind_context.GetBinding(RelationAlias(vector<string>(names.begin(), names.end() - 1)), names.back(),
		                                 error) != nullptr;
	}
	if (column || names.size() > 3) {
		return nullptr;
	}
	return bind_context.GetBinding(RelationAlias(names), error);
}

//! A relation's stored columns as key elements, registered with the scan.
static vector<DecideKeyElement> BindRelationColumns(Binding &relation) {
	KeyColumns columns;
	AddRelationColumns(relation, columns);
	return KeyElements(columns);
}

bool ContainsDecideAggregate(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (!func.is_operator &&
		    (IsDecideAggregateName(func.function_name) || StringUtil::CIEquals(func.function_name, "norm"))) {
			return true;
		}
		// A condition, a key or a `by` key is not part of the value.
		if (func.is_operator && !func.children.empty() &&
		    (func.function_name == REDUCER_BY_TAG || IsDecideScopeMarker(expr))) {
			return ContainsDecideAggregate(*func.children[0]);
		}
		for (auto &child : func.children) {
			if (ContainsDecideAggregate(*child)) {
				return true;
			}
		}
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::OPERATOR) {
		auto &op = expr.Cast<const OperatorExpression>();
		for (auto &child : op.children) {
			if (ContainsDecideAggregate(*child)) {
				return true;
			}
		}
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::CAST) {
		auto &cast = expr.Cast<const CastExpression>();
		return ContainsDecideAggregate(*cast.child);
	}
	return false;
}

// Names that can legally wrap a DECIDE variable at bind time. ABS is
// linearized by the optimizer; POWER/POW(expr, 2) feeds the QP objective path;
// SUM/AVG/MIN/MAX/COUNT are aggregates handled in BindAggregate; +, -, *, /,
// **, and unary tags fall through to the normal linear-extraction path.
// Including aggregate names prevents false-flagging the IN-rewrite's "+"
// nodes (synthesized as non-operator FunctionExpression) or nested aggregates.
static bool IsAllowedNameOverDecideVar(const string &name) {
	auto lname = StringUtil::Lower(name);
	if (lname == "abs" || lname == "power" || lname == "pow") return true;
	if (lname == "norm") return true; // bound as an optimizer-owned aggregate marker below
	if (lname == "sum" || lname == "avg" || lname == "min" || lname == "max") return true;
	if (lname == "count" || lname == "count_star") return true;
	if (lname == "+" || lname == "-" || lname == "*" || lname == "/" || lname == "**") return true;
	// DecidB-internal tag operators (__when_constraint__, __per_constraint__, ...)
	// are synthesized as FunctionExpression and handled by dedicated binders.
	if (name.size() >= 4 && name.substr(0, 2) == "__" && name.substr(name.size() - 2) == "__") return true;
	return false;
}

// True when the function name resolves to an AGGREGATE in the catalog. These
// are rejected (with an aggregate-specific error) in BindAggregate, so we must
// not also false-flag them here as "non-linear scalars".
static bool IsAggregateFunctionName(ClientContext &context, const FunctionExpression &func) {
	auto entry = Catalog::GetEntry(context, CatalogType::SCALAR_FUNCTION_ENTRY,
	                                func.catalog, func.schema, func.function_name,
	                                OnEntryNotFound::RETURN_NULL);
	return entry && entry->type == CatalogType::AGGREGATE_FUNCTION_ENTRY;
}

// Reject any non-linear scalar function that wraps a DECIDE variable. Mirrors
// the COUNT(decide_var) guard in BindAggregate: catch the mis-use at bind time
// with a semantic-specific message instead of letting it slip through to
// per-row execution, which would silently strip the scalar and produce wrong
// answers. Functions that only wrap table columns are fine — the
// wrapper folds to a per-row constant before the solver sees it.
// True if this function is POWER / POW / **. These share the same validation:
// exponent must be a constant numeric 2. All other exponents are non-linear
// (fractional → radicals, negative → reciprocal, variable → exponential),
// or unsupported higher-degree integer powers (3+ → cubic and above).
static bool IsPowerFunction(const FunctionExpression &func) {
	auto lname = StringUtil::Lower(func.function_name);
	return lname == "power" || lname == "pow" || lname == "**";
}

// Reject POWER(base, exp) when base contains a decide variable and exp is not
// a constant numeric 2. Mirrors ValidateQuadraticPower's error messages so
// test matchers and user-facing wording are consistent across SUM and non-SUM
// contexts. Running at bind time keeps a bad exponent from reaching the
// quadratic extractor, which recognizes only POWER(linear_expr, 2).
static void ValidatePowerExponent(const FunctionExpression &func,
                                  const case_insensitive_map_t<idx_t> &variables) {
	if (func.children.size() != 2) {
		return; // Arity errors are surfaced elsewhere with clearer messages.
	}
	// Only gate POWER whose base transitively references a DECIDE variable.
	// POWER over pure data columns folds to a per-row constant and is fine.
	if (!ExpressionContainsDecideVariable(*func.children[0], variables)) {
		return;
	}
	auto &exponent = *func.children[1];
	if (exponent.GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw BinderException(
		    "POWER exponent in DECIDE expression must be a constant integer "
		    "(only 2 is supported)");
	}
	auto &exp_const = exponent.Cast<const ConstantExpression>();
	double exp_val;
	try {
		exp_val = exp_const.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	} catch (...) {
		throw BinderException("POWER exponent must be numeric");
	}
	if (exp_val != 2.0) {
		throw BinderException(
		    "Only POWER(expr, 2) is supported for quadratic expressions. "
		    "Found exponent %g. Higher powers are not allowed.", exp_val);
	}
}

// True if `expr` contains an aggregate over table columns only (no DECIDE
// variable anywhere in its argument) — e.g. `AVG(price)`, `MIN(cost)`. Such a
// value needs the whole row set, so it is a scalar the query must pre-compute,
// not a per-row coefficient. Recurses so nested forms (`AVG(p) * 2`) are caught.
static bool ContainsDataOnlyAggregate(ClientContext &context, const ParsedExpression &expr,
                                      const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (IsAggregateFunctionName(context, func) &&
		    !ExpressionContainsDecideVariable(expr, variables)) {
			return true;
		}
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (ContainsDataOnlyAggregate(context, child, variables)) {
			found = true;
		}
	});
	return found;
}

void ValidateDecideNoNonLinearScalar(ClientContext &context,
                                     const ParsedExpression &expr,
                                     const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (IsPowerFunction(func)) {
			ValidatePowerExponent(func, variables);
		} else if (func.is_operator && func.function_name == "*" &&
		           ExpressionContainsDecideVariable(expr, variables) &&
		           ContainsDataOnlyAggregate(context, expr, variables)) {
			// A data-only aggregate multiplied by a decision variable
			// (`SUM(avg(p) * x)`) is not a per-row coefficient: the aggregate
			// spans all rows. Symbolic normalization would silently distribute the
			// variable into it (`avg(p)*x` -> `avg(p*x)`, a different constraint) and
			// return a wrong optimum, so reject it here — before normalization —
			// consistent with the direct `SUM(sum(p) * x)` rejection.
			throw BinderException(
			    "An aggregate over table columns (e.g. AVG(col), MIN(col)) cannot multiply a "
			    "decision variable. Pre-compute it as a scalar in a subquery or CTE and reference "
			    "that value.");
		} else if (func.is_operator && func.function_name == "/") {
			// Division is only linear when the divisor contains no decide
			// variable. x / y (decide vars in divisor) is non-linear; catch
			// it here so it doesn't fall through to per-row extraction
			// (which would silently produce wrong results) or symbolic
			// normalization (which would throw InternalException).
			if (func.children.size() == 2 &&
			    ExpressionContainsDecideVariable(*func.children[1], variables)) {
				throw BinderException(
				    "Division by a DECIDE variable is not supported: "
				    "it would make the model non-linear. The divisor must "
				    "not reference a decision variable.");
			}
			// A data-only aggregate stays on the bound side (S10). Like the multiplier
			// above, it may not share a scaled term with a decision: as the divisor, or
			// inside the dividend (`(sum(x) + sum(cap)) / 2`).
			if (func.children.size() == 2 && ExpressionContainsDecideVariable(expr, variables) &&
			    ContainsDataOnlyAggregate(context, expr, variables)) {
				throw BinderException(
				    "An aggregate over table columns (e.g. AVG(col), MIN(col)) cannot %s a decision "
				    "variable. Pre-compute it as a scalar in a subquery or CTE and reference that value.",
				    ContainsDataOnlyAggregate(context, *func.children[1], variables) ? "divide"
				                                                                     : "be divided together with");
			}
		} else if (!IsAllowedNameOverDecideVar(func.function_name) &&
		           !IsAggregateFunctionName(context, func)) {
			for (auto &child : func.children) {
				if (ExpressionContainsDecideVariable(*child, variables)) {
					throw BinderException(
					    "Scalar function '%s' over a DECIDE variable is not supported: "
					    "it would make the model non-linear. Only ABS() and POWER(..., 2) "
					    "can wrap a decision variable.",
					    func.function_name);
				}
			}
		}
	}
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		ValidateDecideNoNonLinearScalar(context, child, variables);
	});
}

//! Does this node reduce whatever is under it to an integer, whatever that operand's
//! declared type is? Only `norm(e, 0, M)` does: the L0 "norm" counts how many entries
//! are nonzero, so its value is a count and lands on the integer lattice even for a
//! REAL `e`. That is the definition of L0, not a property of how the count is later
//! linearized, so the integer-step rewrites are exact over it.
//!
//! The other orders are not counts and do not qualify: `norm(e, 1)` sums magnitudes,
//! `norm(e, 2)` is a Euclidean length, `norm(e, 'inf')` is a maximum — each is as
//! continuous as `e` is.
static bool IsIntegerValuedReducer(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<const FunctionExpression>();
	if (func.is_operator || !StringUtil::CIEquals(func.function_name, "norm")) {
		return false;
	}
	// Shape is validated properly when the marker is bound; here an unparseable order
	// simply is not the L0 case, and the real diagnosis follows from the binder.
	if (func.children.size() < 2 || func.children[1]->GetExpressionClass() != ExpressionClass::CONSTANT) {
		return false;
	}
	auto &order_value = func.children[1]->Cast<const ConstantExpression>().value;
	if (!order_value.type().IsNumeric()) {
		return false; // 'inf'
	}
	try {
		return order_value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>() == 0.0;
	} catch (...) {
		return false;
	}
}

//! Name of the first REAL-declared decision variable referenced by `expr`, as the
//! user spelled it, or empty if there is none. Walks scalar-subquery bodies for the
//! same reason ValidateDecideNoExplicitDecisionCasts does: a correlated decision
//! reference can sit inside one, and EnumerateChildren does not enter a QueryNode.
//!
//! Stops at an integer-valued reducer: what a comparison compares is the reducer's
//! value, so a REAL decision counted by `norm(..., 0, M)` is not the compared
//! quantity and must not be reported as one.
static string FindRealDecideVariable(const ParsedExpression &expr,
                                     const case_insensitive_map_t<idx_t> &variables,
                                     const vector<LogicalType> &variable_types) {
	if (IsIntegerValuedReducer(expr)) {
		return "";
	}
	if (IsVariableExpression(expr, variables)) {
		const auto &colref = expr.Cast<const ColumnRefExpression>();
		auto it = variables.find(colref.GetColumnName());
		if (it != variables.end() && it->second < variable_types.size()) {
			const auto &type = variable_types[it->second];
			if (type == LogicalType::DOUBLE || type == LogicalType::FLOAT) {
				return colref.GetColumnName();
			}
		}
	}

	string found;
	EnumerateChildrenAndSubqueries(expr, [&](const ParsedExpression &child) {
		if (found.empty()) {
			found = FindRealDecideVariable(child, variables, variable_types);
		}
	});
	return found;
}

void ValidateDecideNoIntegerStepComparisonOnReal(const ParsedExpression &expr,
                                                 const case_insensitive_map_t<idx_t> &variables,
                                                 const vector<LogicalType> &variable_types) {
	// Only comparisons in constraint position are checked — the ones that become
	// model rows and would therefore be encoded by stepping the bound. A comparison
	// nested inside an operand is a boolean value, not a constraint: in
	// `(sum(x) > 1) + 3 <= 10` the `sum(x) > 1` is added to 3, and the type error
	// that names is a better diagnosis than anything said about strictness. So this
	// descends the way DecideConstraintsBinder does — through conjunctions and
	// through the constraint child of a `when` / `per` wrapper — and no further.
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::CONJUNCTION: {
		auto &conj = expr.Cast<const ConjunctionExpression>();
		for (const auto &child : conj.children) {
			ValidateDecideNoIntegerStepComparisonOnReal(*child, variables, variable_types);
		}
		return;
	}
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<const FunctionExpression>();
		if (IsDecideScopeMarker(expr) && !func.children.empty()) {
			// children[0] is the constraint; the rest are the condition / key.
			ValidateDecideNoIntegerStepComparisonOnReal(*func.children[0], variables, variable_types);
		}
		return;
	}
	case ExpressionClass::COMPARISON: {
		auto type = expr.GetExpressionType();
		bool is_strict =
		    type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_GREATERTHAN;
		bool is_not_equal = type == ExpressionType::COMPARE_NOTEQUAL;
		if (is_strict || is_not_equal) {
			auto &comp = expr.Cast<const ComparisonExpression>();
			// Both sides, because canonicalization has not run yet: the user may have
			// written `5 > x` as readily as `x < 5`, and reading a side is not moving one.
			string real_var = FindRealDecideVariable(*comp.left, variables, variable_types);
			if (real_var.empty()) {
				real_var = FindRealDecideVariable(*comp.right, variables, variable_types);
			}
			if (real_var.empty()) {
				return;
			}
			if (is_not_equal) {
				// No repair by stepping exists here, unlike the strict case: excluding a
				// single point from a continuous range removes nothing a solver can act
				// on. So the advice is to change the declared type or to name the range
				// the user actually wants excluded.
				//
				// Spelled by hand rather than through comp.ToString(), which renders
				// COMPARE_NOTEQUAL as `!=`. The clause is quoted back to the user, so it
				// has to read the way they wrote it.
				string clause = "(" + comp.left->ToString() + " <> " + comp.right->ToString() + ")";
				throw BinderException(
				    comp,
				    "Inequality '<>' is not supported over the REAL decision '%s': '%s'. "
				    "A REAL decision can take values arbitrarily close to the compared value "
				    "on either side, so excluding that single point rules out nothing. "
				    "Declare '%s' as INT if the quantity is a whole number, or use '<=' / '>=' "
				    "to exclude the range you actually mean.",
				    real_var, clause, real_var);
			}
			bool is_less = type == ExpressionType::COMPARE_LESSTHAN;
			throw BinderException(
			    comp,
			    "Strict inequality '%s' is not supported over the REAL decision '%s': '%s'. "
			    "A REAL decision takes any value up to the bound, so there is no next value "
			    "to stop at. Use '%s' instead, and move the bound by one step if the compared "
			    "quantity is a count or other whole number.",
			    is_less ? "<" : ">", real_var, comp.ToString(), is_less ? "<=" : ">=");
		}
		return;
	}
	default:
		return;
	}
}

//===--------------------------------------------------------------------===//
// Integrality of a compared side, from declared types
//===--------------------------------------------------------------------===//

//! Does this side reference a decision variable? Only such a side is a *compared
//! quantity*; a side made of constants and data columns is the bound `K`.
//!
//! `K` is deliberately exempt from the integrality refusal, because a fractional bound is
//! not an error: no whole number equals 2.5, so `SUM(x) <> 2.5` is a tautology and the
//! rewrite drops it, and an infinite bound reads the same way. Refusing here would turn
//! two well-defined outcomes into errors. This mirrors the value-based guards this
//! validator replaced, which read `variable_indices` and `row_coefficients` and never
//! looked at `rhs_values`.
static bool SideReferencesDecision(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<const BoundColumnRefExpression>();
		if (colref.binding.table_index == decide_index) {
			return true;
		}
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = SideReferencesDecision(child, decide_index);
		}
	});
	return found;
}

//! Does this type guarantee whole-numbered values?
//!
//! `DECIMAL` carries its scale in the type, so `DECIMAL(15, 0)` qualifies and
//! `DECIMAL(15, 2)` does not — the latter regardless of what its rows actually hold.
//! No floating type qualifies: `DOUBLE` can hold `3.0`, but nothing in the type says it
//! must, and a rule that depended on the stored values would make a query's validity
//! change when a row is inserted.
static bool DecideTypeIsWholeNumbered(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
		return true;
	case LogicalTypeId::DECIMAL:
		return DecimalType::GetScale(type) == 0;
	default:
		return false;
	}
}

//! Is this bound node the inert `norm(e, 0, M)` marker? The L0 "norm" counts nonzeros,
//! so its value is a count whatever `e` is typed as. `NORM_MARKER_TAG_PREFIX` is followed
//! by the order, so an L0 marker's tag begins `__decide_norm_0`.
static bool IsBoundL0NormMarker(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	string payload;
	if (!ExtractDecideTagPayload(expr.alias, NORM_MARKER_TAG_PREFIX, payload)) {
		return false;
	}
	return StringUtil::StartsWith(payload, "0");
}

//! Is this bound node an AVG aggregate?
static bool IsBoundAvgAggregate(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	auto &agg = expr.Cast<const BoundAggregateExpression>();
	return StringUtil::CIEquals(agg.function.name, "avg");
}

static bool DecideSideIsWholeNumbered(const Expression &expr, bool allow_avg_hoist, idx_t decide_index,
                                      bool addends_count = false);

//! Is a MIN or MAX of `body`, standing on the side's additive spine, whole-numbered? Its constant addends move onto
//! the bound in stage 04 (SplitExtremumBody), so only the terms that stay count.
static bool DecideExtremumBodyIsWholeNumbered(const Expression &body, idx_t decide_index) {
	for (auto &term : SplitExtremumBody(body, decide_index).kept) {
		if (!DecideSideIsWholeNumbered(*term.second, false, decide_index, /*addends_count=*/true)) {
			return false;
		}
	}
	return true;
}

//! Is `expr` provably whole-numbered from declared types alone?
//!
//! `allow_avg_hoist` is set only for `<>`, whose rewrite multiplies both sides by the
//! group size and so compares `SUM(e)` against `K*n` — integral whenever `e` is. A
//! strict comparison has no such hoist and keeps AVG's fractional 1/n coefficients.
//!
//! `addends_count` is set where a decision-free addend is part of the value rather than
//! of the bound: inside a MIN or MAX, whose strict bound is stepped on the extremum itself
//! (stage 06) once its constant addends are moved out (DecideExtremumBodyIsWholeNumbered), and
//! under anything that is not linear in it -- a power, a product of decisions, a norm of order
//! 1, 2 or 'inf' (`|x + 0.5|`, `(x + 0.25)^2`, `(x + 0.5) * y` are not whole).
static bool DecideSideIsWholeNumbered(const Expression &expr, bool allow_avg_hoist, idx_t decide_index,
                                      bool addends_count) {
	if (IsBoundL0NormMarker(expr)) {
		return true;
	}
	if (allow_avg_hoist && IsBoundAvgAggregate(expr)) {
		auto &agg = expr.Cast<const BoundAggregateExpression>();
		return agg.children.size() == 1 &&
		       DecideSideIsWholeNumbered(*agg.children[0], false, decide_index, addends_count);
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CAST: {
		// A cast to a whole-numbered type makes the value one; a widening cast of an
		// already-whole operand (the binder inserts many, e.g. INTEGER -> DECIMAL(18,0))
		// keeps it one. Either direction suffices.
		auto &cast = expr.Cast<const BoundCastExpression>();
		return DecideTypeIsWholeNumbered(cast.return_type) ||
		       DecideSideIsWholeNumbered(*cast.child, allow_avg_hoist, decide_index, addends_count);
	}
	case ExpressionClass::BOUND_AGGREGATE: {
		// SUM / MIN / MAX of whole numbers are whole, and so is a norm of order 1, 2 or
		// 'inf' (a sum of absolute values or squares, a largest absolute value) of a whole
		// argument. AVG is handled above; anything else falls through to its declared
		// return type.
		auto &agg = expr.Cast<const BoundAggregateExpression>();
		const auto &name = agg.function.name;
		bool extremum = StringUtil::CIEquals(name, "min") || StringUtil::CIEquals(name, "max");
		bool norm = HasDecideTag(agg.alias, NORM_MARKER_TAG_PREFIX);
		if (extremum && agg.children.size() == 1) {
			// On the side's additive spine its constant addends move to the bound (stage 04); under a power or a
			// product of decisions they stay inside, and the whole body counts.
			return addends_count ? DecideSideIsWholeNumbered(*agg.children[0], false, decide_index, true)
			                     : DecideExtremumBodyIsWholeNumbered(*agg.children[0], decide_index);
		}
		if (StringUtil::CIEquals(name, "sum") && agg.children.size() == 1) {
			return DecideSideIsWholeNumbered(*agg.children[0], false, decide_index, addends_count || norm);
		}
		return DecideTypeIsWholeNumbered(expr.return_type);
	}
	case ExpressionClass::BOUND_FUNCTION: {
		// Sums, differences and products of whole numbers are whole. Division is not,
		// and neither is anything else, so both fall through to the return type.
		auto &func = expr.Cast<const BoundFunctionExpression>();
		const auto &name = func.function.name;
		if (name == "+" || name == "-") {
			// An addend that references no decision is part of the bound, not of the
			// compared quantity: `x + 1000003.50 < K` is exactly `x < K - 1000003.50`,
			// and stepping that is still exact because `x` is still on the lattice. A
			// fractional offset moves the bound; it does not move the lattice. (The
			// value-based guard this replaces skipped constant terms for the same
			// reason — it only ever read coefficients carrying a variable index.)
			for (const auto &child : func.children) {
				if (!addends_count && !SideReferencesDecision(*child, decide_index)) {
					continue;
				}
				if (!DecideSideIsWholeNumbered(*child, allow_avg_hoist, decide_index, addends_count)) {
					return false;
				}
			}
			return true;
		}
		if (name == "*") {
			// Every factor matters here, decision-bearing or not: a fractional
			// multiplier rescales the decision off the lattice (`0.5 * x`). A product of
			// decisions is not linear in either factor's addends.
			idx_t decision_factors = 0;
			for (const auto &child : func.children) {
				decision_factors += SideReferencesDecision(*child, decide_index);
			}
			for (const auto &child : func.children) {
				if (!DecideSideIsWholeNumbered(*child, allow_avg_hoist, decide_index,
				                               addends_count || decision_factors > 1)) {
					return false;
				}
			}
			return true;
		}
		// A whole number raised to a whole, non-negative power is whole, so `POWER(x, 2)`
		// stays on the lattice even though the function's return type is DOUBLE. The
		// declared return type is the wrong thing to read here for the same reason it is
		// wrong for an L0 count. A fractional or negative exponent is not a polynomial
		// degree at all and `ValidatePowerExponent` rejects it, so it need not be
		// answered here.
		if (name == "pow" || name == "power" || name == "**" || name == "^") {
			double exp_val;
			if (func.children.size() == 2 && TryGetDecideConstantExponent(*func.children[1], exp_val) &&
			    exp_val >= 0.0 && exp_val == std::floor(exp_val)) {
				return DecideSideIsWholeNumbered(*func.children[0], allow_avg_hoist, decide_index, true);
			}
			return false;
		}
		return DecideTypeIsWholeNumbered(expr.return_type);
	}
	default:
		return DecideTypeIsWholeNumbered(expr.return_type);
	}
}

bool DecideValueIsWholeNumbered(const Expression &expr, idx_t decide_index) {
	return DecideSideIsWholeNumbered(expr, false, decide_index, /*addends_count=*/true);
}

//! The operand a refusal should talk about.
struct FractionalOperand {
	string name;
	LogicalType type;
	//! A column can be cast to a whole-numbered type, and the message says so. An
	//! intrinsically fractional expression (`AVG(x)`, a division) cannot — casting it
	//! would truncate the value rather than state an assumption about it — so that
	//! message offers the non-strict operator instead.
	bool is_castable_column = false;
};

//! First operand that is not whole-numbered, for the message. Descends the same way the
//! test above does, so what it reports is what was refused.
static bool FindFractionalOperand(const Expression &expr, bool allow_avg_hoist, idx_t decide_index,
                                  optional_ptr<const DecideSourceNames> names, FractionalOperand &out) {
	if (DecideSideIsWholeNumbered(expr, allow_avg_hoist, decide_index)) {
		return false;
	}
	// Prefer a named leaf over the composite node above it: `l_quantity` is actionable,
	// `sum((x * l_quantity))` is not.
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = FindFractionalOperand(child, allow_avg_hoist, decide_index, names, out);
		}
	});
	if (found) {
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		out.name = expr.Cast<const BoundColumnRefExpression>().GetName();
		out.type = expr.return_type;
		out.is_castable_column = true;
		return true;
	}
	// No fractional leaf underneath: the node itself is what produces fractions, named as
	// written (its reducer's alias carries the source tags).
	out.name = names ? RenderDecideSource(expr, *names) : expr.GetName();
	out.type = expr.return_type;
	out.is_castable_column = false;
	return true;
}

void ValidateDecideIntegralComparisonOperands(const Expression &expr, idx_t decide_index,
                                              optional_ptr<const DecideSourceNames> names) {
	// Constraint position only, and by the same reasoning as the parsed-tree validator:
	// a comparison nested inside an operand is a boolean value, not a model row.
	// ForEachConstraintLeaf owns that descent; this is only the check.
	ForEachConstraintLeaf(expr, [&](const Expression &node) {
		if (node.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return;
		}
		auto type = node.GetExpressionType();
		bool is_strict =
		    type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_GREATERTHAN;
		bool is_not_equal = type == ExpressionType::COMPARE_NOTEQUAL;
		if (!is_strict && !is_not_equal) {
			return;
		}
		auto &comp = node.Cast<const BoundComparisonExpression>();
		// Only a side that carries decisions is a compared quantity; the other is the
		// bound, and a fractional bound is a tautology rather than an error.
		bool left_is_compared = SideReferencesDecision(*comp.left, decide_index);
		bool right_is_compared = SideReferencesDecision(*comp.right, decide_index);
		FractionalOperand bad;
		if (!(left_is_compared && FindFractionalOperand(*comp.left, is_not_equal, decide_index, names, bad)) &&
		    !(right_is_compared && FindFractionalOperand(*comp.right, is_not_equal, decide_index, names, bad))) {
			return;
		}
		string op = is_not_equal ? "<>" : (type == ExpressionType::COMPARE_LESSTHAN ? "<" : ">");
		string alternative = is_not_equal ? "'<=' or '>='"
		                                  : (type == ExpressionType::COMPARE_LESSTHAN ? "'<='" : "'>='");
		string why = is_not_equal
		                 ? "the rewrite (x <> K becomes x <= K-1 OR x >= K+1) is exact only on "
		                   "whole numbers"
		                 : "stepping the bound is exact only on whole numbers";
		// Pass the comparison so the message carries a caret. The bound tree only started
		// carrying source locations when DecideBinder::BindExpression began stamping them;
		// before that these two throws named no node because there was nothing to name.
		if (bad.is_castable_column) {
			throw BinderException(
			    comp,
			    "Comparison '%s' is not supported here: column '%s' has type %s, which allows "
			    "fractional values, and %s. If '%s' holds whole numbers, cast it "
			    "(%s::BIGINT); otherwise compare with %s.",
			    op, bad.name, bad.type.ToString(), why, bad.name, bad.name, alternative);
		}
		throw BinderException(
		    comp,
		    "Comparison '%s' is not supported here: '%s' has type %s and can take fractional "
		    "values, and %s. Compare with %s instead.",
		    op, bad.name, bad.type.ToString(), why, alternative);
	});
}

string DecideWrittenFunctionName(const FunctionExpression &func) {
	return StringUtil::CIEquals(func.function_name, "count_star") ? "count(*)" : func.function_name;
}

const char *DecideCaseUnsupportedMessage() {
	return "CASE expressions are not supported inside DECIDE constraints or "
	       "objectives. Write the condition as a scope in front of the clause "
	       "(when category = 'A': sum(x) >= 1), split rows into classes with per, "
	       "or pre-compute conditional values in a CTE or subquery before the "
	       "DECIDE clause.";
}

// Forward declaration — needed because ValidateQuadraticPower calls ValidateSumArgumentInternal.
static bool ValidateSumArgumentInternal(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                        bool &has_decide_variable, string &error_msg, bool allow_quadratic = false);

// Shared validation for POWER(expr, 2) and expr ** 2 patterns.
// Returns true if the pattern is a valid quadratic form; sets has_decide_variable.
// The base expression is validated as strictly linear (allow_quadratic=false) to prevent nesting.
static bool ValidateQuadraticPower(vector<unique_ptr<ParsedExpression>> &children,
                                   const case_insensitive_map_t<idx_t> &variables,
                                   bool &has_decide_variable, string &error_msg, const string &label) {
	if (children.size() != 2) {
		error_msg = label + " requires exactly two arguments";
		return false;
	}
	auto &exponent = *children[1];
	if (exponent.GetExpressionClass() != ExpressionClass::CONSTANT) {
		error_msg = "POWER exponent in DECIDE expression must be a constant integer (only 2 is supported)";
		return false;
	}
	auto &exp_const = exponent.Cast<ConstantExpression>();
	double exp_val;
	try {
		exp_val = exp_const.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	} catch (...) {
		error_msg = "POWER exponent must be numeric";
		return false;
	}
	if (exp_val != 2.0) {
		error_msg = StringUtil::Format(
		    "Only POWER(expr, 2) is supported for quadratic expressions. "
		    "Found exponent %g. Higher powers are not allowed.", exp_val);
		return false;
	}
	// Validate the base is a LINEAR expression (allow_quadratic=false prevents nesting like POWER(POWER(x,2),2))
	bool base_has_var = false;
	if (!ValidateSumArgumentInternal(*children[0], variables, base_has_var, error_msg, /*allow_quadratic=*/false)) {
		error_msg = "Inside " + label + ": " + error_msg;
		return false;
	}
	if (!base_has_var) {
		error_msg = label + " in DECIDE expression must reference at least one DECIDE variable";
		return false;
	}
	has_decide_variable = true;
	return true;
}

static bool ValidateSumArgumentInternal(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                        bool &has_decide_variable, string &error_msg,
                                        bool allow_quadratic) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
        if (IsVariableExpression(expr, variables)) {
            has_decide_variable = true;
        }
        return true;
    }
    case ExpressionClass::CONSTANT:
        return true;
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		string func_name_lower = StringUtil::Lower(func.function_name);
		if (!func.is_operator) {
			if (func_name_lower == "abs") {
				// ABS is allowed inside SUM — will be linearized by the optimizer.
				// Treat ABS as opaque: just verify it references a decide variable.
				if (func.children.size() != 1) {
					error_msg = "ABS requires exactly one argument";
					return false;
				}
				if (ExpressionContainsDecideVariable(*func.children[0], variables)) {
					has_decide_variable = true;
				}
				return true;
			}
			if (func_name_lower == "power" || func_name_lower == "pow") {
				if (!ExpressionContainsDecideVariable(expr, variables)) {
					// POWER over data columns only — a per-row constant, handled by the
					// same rule as any other data-only scalar function below. The
					// quadratic gate exists for POWER over a DECIDE variable; applying it
					// here rejects `SUM(x * POWER(qty, 2))`, which is linear in x.
					return true;
				}
				if (!allow_quadratic) {
					error_msg = "SUM expression must remain linear in DECIDE variables — "
					            "POWER(expr, 2) is only allowed in objectives, not constraints";
					return false;
				}
				return ValidateQuadraticPower(func.children, variables, has_decide_variable, error_msg, "POWER(..., 2)");
			}
			if (func_name_lower == "min" || func_name_lower == "max" || func_name_lower == "sum" || func_name_lower == "avg") {
				// Nested aggregates (the two-level objective `sum(per k: max(e) by (k))`) are allowed.
				// The optimizer will detect and rewrite them.
				if (func.children.size() == 1 && ExpressionContainsDecideVariable(*func.children[0], variables)) {
					has_decide_variable = true;
					return true;
				}
				error_msg = StringUtil::Format("Nested %s() inside DECIDE expression must reference a DECIDE variable",
				                               StringUtil::Upper(func_name_lower));
				return false;
			}
			if (!ExpressionContainsDecideVariable(expr, variables)) {
				// Data-only named scalar function on table columns (e.g. mod(id, 97),
				// floor(price)). It folds into a per-row coefficient during
				// symbolization and contributes no DECIDE variable. Data-only
				// aggregates (SUM/AVG/MIN/MAX of columns only) were already rejected
				// above — they require a DECIDE variable — so they never reach here.
				return true;
			}
			error_msg = StringUtil::Format("Unsupported function '%s' inside DECIDE SUM expression", func.function_name);
			return false;
		}
		if (func_name_lower == "**") {
			if (!ExpressionContainsDecideVariable(expr, variables)) {
				return true;
			}
			if (!allow_quadratic) {
				error_msg = "SUM expression must remain linear in DECIDE variables — "
				            "expr ** 2 is only allowed in objectives, not constraints";
				return false;
			}
			return ValidateQuadraticPower(func.children, variables, has_decide_variable, error_msg, "expr ** 2");
		}
		if (func_name_lower == "-") {
			for (auto &child : func.children) {
				if (!ValidateSumArgumentInternal(*child, variables, has_decide_variable, error_msg, allow_quadratic)) {
					return false;
				}
			}
			return true;
		}
		if (func_name_lower == "/") {
			// Division is linear iff the divisor contains no decide variable
			// (numerator can reference decide vars; dividing by a data
			// expression is just a coefficient scale). x / y where both
			// sides are decide vars is non-linear and rejected here.
			if (func.children.size() != 2) {
				error_msg = "Division requires exactly two arguments";
				return false;
			}
			if (ExpressionContainsDecideVariable(*func.children[1], variables)) {
				error_msg = "Division by a DECIDE variable is not supported "
				            "(would make the model non-linear)";
				return false;
			}
			return ValidateSumArgumentInternal(*func.children[0], variables,
			                                   has_decide_variable, error_msg,
			                                   allow_quadratic);
		}
		if (func_name_lower == "*" || func_name_lower == "+") {
			for (auto &child : func.children) {
				if (!ValidateSumArgumentInternal(*child, variables, has_decide_variable, error_msg, allow_quadratic)) {
					return false;
				}
			}
			// Degree is deliberately not judged here. It is one concept with one owner —
			// `ValidateDecideConstraintDegree` / `ValidateDecideObjectiveDegree`, which run on
			// the bound tree once binding is complete — so that a per-row constraint and a
			// reducer argument are held to the same rule. This branch used to re-derive it,
			// and because its name scoped it to SUM arguments nothing called it for
			// `POWER(x*y, 2) <= 5`, which then reached term extraction at layer 5.
			return true;
		}
		// An operator the model doesn't understand (`%`, bitwise ops, …) is still
		// fine if this subexpression references no DECIDE variable: it's a per-row
		// constant coefficient: canonicalization keeps it whole as one atom and the
		// physical layer evaluates it as ordinary data.
		// Only genuinely variable-bearing uses of an unsupported operator are rejected.
		if (!ExpressionContainsDecideVariable(expr, variables)) {
			return true;
		}
		error_msg = StringUtil::Format("Unsupported operator '%s' inside DECIDE SUM expression", func.function_name);
		return false;
	}
	case ExpressionClass::OPERATOR: {
		// COALESCE/IFNULL and other parsed operator nodes are ordinary per-row
		// coefficients when every child is data-only. Keep the same boundary as
		// unsupported FunctionExpression operators above: DuckDB evaluates the
		// opaque data expression, while DECIDE only models expressions that touch a
		// decision variable.
		if (!ExpressionContainsDecideVariable(expr, variables)) {
			return true;
		}
		error_msg = StringUtil::Format(
		    "Unsupported operator expression over a DECIDE variable inside DECIDE SUM expression: %s",
		    expr.ToString());
		return false;
	}
	case ExpressionClass::CAST: {
		auto &cast = expr.Cast<CastExpression>();
		return ValidateSumArgumentInternal(*cast.child, variables, has_decide_variable, error_msg, allow_quadratic);
	}
    case ExpressionClass::SUBQUERY: {
        auto &subquery = expr.Cast<SubqueryExpression>();
        if (subquery.subquery_type != SubqueryType::SCALAR) {
            error_msg = "Subquery in DECIDE SUM expression must be scalar";
            return false;
        }
        if (ExpressionContainsDecideVariable(expr, variables)) {
            error_msg = "Subquery in DECIDE SUM expression cannot contain DECIDE variables";
            return false;
        }
        return true;
    }
    case ExpressionClass::CASE:
        error_msg = DecideCaseUnsupportedMessage();
        return false;
    default:
		// Any other shape that reads no decision (a comparison, a parameter, a lambda) is a
		// per-row coefficient DuckDB evaluates, like the data-only operators above.
		if (!ExpressionContainsDecideVariable(expr, variables)) {
			return true;
		}
		error_msg = StringUtil::Format("SUM expression does not support '%s'", expr.ToString());
		return false;
    }
}

bool ValidateSumArgument(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables, string &error_msg,
                         bool allow_quadratic) {
	bool has_decide_variable = false;
	if (!ValidateSumArgumentInternal(expr, variables, has_decide_variable, error_msg, allow_quadratic)) {
		return false;
	}
	if (!has_decide_variable) {
		error_msg = "SUM expression must reference at least one DECIDE variable";
		return false;
	}
	return true;
}

DecideBinder::DecideBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables,
                           const case_insensitive_set_t &scalar_variables,
                           optional_ptr<DecideQualifierContext> qualifier_context)
    : ExpressionBinder(binder, context), variables(variables), scalar_variables(scalar_variables),
      qualifier_context(qualifier_context) {
    is_top_expression = true;
}

BindResult DecideBinder::PreserveQueryLocation(optional_idx location, BindResult result) {
	// Only fill an empty location: a nested bind that already recorded a more precise
	// node should keep it rather than be widened to its parent's span.
	if (!result.HasError() && result.expression && !result.expression->GetQueryLocation().IsValid()) {
		result.expression->SetQueryLocation(location);
	}
	return result;
}

BindResult DecideBinder::ThrowClauseError(BindResult result, bool root_expression) {
	if (root_expression && result.HasError()) {
		auto subtype = result.error.ExtraInfo().find("error_subtype");
		if (subtype == result.error.ExtraInfo().end() ||
		    (subtype->second != "COLUMN_NOT_FOUND" && subtype->second != "TABLE_NOT_FOUND")) {
			result.error.Throw();
		}
	}
	return result;
}

bool DecideBinder::IsScalarDecideVariable(const ParsedExpression &expr) const {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	const auto &colref = expr.Cast<const ColumnRefExpression>();
	// A decision is referenced by its bare name (the declarations binder refuses `D.x`),
	// so only the bare name can name a query-wide one.
	return !colref.IsQualified() && scalar_variables.count(colref.GetColumnName()) > 0;
}

bool DecideBinder::IsDataOperand(const ParsedExpression &expr) const {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::FUNCTION:
	case ExpressionClass::SUBQUERY:
	case ExpressionClass::CASE:
		return false;
	default:
		return !ExpressionContainsDecideVariable(expr, variables);
	}
}

bool DecideBinder::IsRowInvariantExpression(const ParsedExpression &expr) const {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		if (!IsVariableExpression(expr, variables)) {
			return false; // a plain data column varies per row
		}
		return IsScalarDecideVariable(expr); // row/entity-scoped decisions vary; scalars don't
	}
	bool invariant = true;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (invariant && !IsRowInvariantExpression(child)) {
			invariant = false;
		}
	});
	return invariant;
}

bool DecideBinder::ClassifyReducerCall(FunctionExpression &func, DecideExpression &result, string &error_msg) {
	auto fname = StringUtil::Lower(func.function_name);
	if (fname == "norm") {
		if (func.children.empty()) {
			error_msg = StringUtil::Format("NORM needs an expression and an order, as in norm(e, 1), found '%s'",
			                               func.ToString());
			result = DecideExpression::INVALID;
		} else if (!ValidateSumArgument(*func.children.front(), variables, error_msg, /*allow_quadratic=*/true)) {
			error_msg += ", found '" + func.ToString() + "'";
			result = DecideExpression::INVALID;
		} else {
			result = DecideExpression::SUM;
		}
		return true;
	}
	if (fname == "sum" || fname == "avg" || fname == "min" || fname == "max") {
		// The value reduced is the argument's body; a `when` or `per` in front of it only
		// chooses the rows.
		auto body = func.children.empty()
		                ? nullptr
		                : &const_cast<ParsedExpression &>(ReducerArgumentBody(*func.children.front()));
		if (body && ExpressionContainsDecideVariable(*body, variables) && IsRowInvariantExpression(*body)) {
			auto body_text = body->ToString();
			error_msg = StringUtil::Format(
			    "%s is a query-wide decision, so %s(%s) has nothing to aggregate over; "
			    "use %s on its own",
			    body_text, StringUtil::Upper(fname), body_text, body_text);
			result = DecideExpression::INVALID;
		} else if (!body) {
			error_msg =
			    StringUtil::Format("%s needs one argument, found '%s'", StringUtil::Upper(fname), func.ToString());
			result = DecideExpression::INVALID;
		} else if (!ValidateSumArgument(*body, variables, error_msg, /*allow_quadratic=*/true)) {
			error_msg += ", found '" + func.ToString() + "'";
			result = DecideExpression::INVALID;
		} else {
			result = DecideExpression::SUM;
		}
		return true;
	}
	return false;
}

BindResult DecideBinder::BindAggregate(FunctionExpression &aggr, AggregateFunctionCatalogEntry &func, idx_t depth) {
	ErrorData error;

	// DECIDE clauses only accept SUM, AVG, MIN, MAX, and COUNT as aggregates. Reject anything
	// else (STRING_AGG, BIT_AND, MEDIAN, HISTOGRAM, etc.) at the canonical DuckDB hook,
	// mirroring WhereBinder::UnsupportedAggregateMessage. `count_star` is permitted because
	// the symbolic phase synthesizes `count_star()` internally when rewriting SUM(constant).
	if (!IsDecideAggregateName(func.name) && func.name != "count" && func.name != "count_star") {
		return BindResult(BinderException::Unsupported(
		    aggr, StringUtil::Format("DECIDE clause does not support aggregate '%s', only SUM, AVG, MIN, MAX, or COUNT is allowed.",
		                             func.name)));
	}

	// COUNT over a DECIDE variable is degenerate: decision variables are never null, so
	// COUNT(x) is identically the row count and does not constrain x. Reject it with a
	// semantic-specific message rather than silently letting it bind to a no-op constraint.
	if (func.name == "count") {
		for (auto &child : aggr.children) {
			if (ExpressionContainsDecideVariable(*child, variables)) {
				return BindResult(BinderException::Unsupported(
				    aggr, "COUNT over a DECIDE variable is degenerate: decision variables are never null, "
				          "so COUNT(x) always equals the row count. Did you mean SUM(x)?"));
			}
		}
	}

	// No Filter/Distinc allowed for Aggregate
	if (aggr.filter || aggr.distinct) {
        return BindResult(BinderException::Unsupported(aggr, StringUtil::Format("DECIDE clause does not support '%s'", aggr.ToString())));
	}

	// Bind arguments
	vector<unique_ptr<Expression>> children;
	vector<LogicalType> child_types;
	for (auto &child_expr : aggr.children) {
        // Use this->BindExpression to ensure subqueries are handled (executed at bind time)
        auto result = BindExpression(child_expr, depth);
        if (result.HasError()) {
            return result;
        }
        auto &bound_child = result.expression;
		child_types.push_back(bound_child->return_type);
		children.push_back(std::move(bound_child));
	}

	// 2. Bind the aggregate function itself
	FunctionBinder function_binder(binder);
	auto best_function_idx = function_binder.BindFunction(func.name, func.functions, child_types, error);
	if (!best_function_idx.IsValid()) {
		error.AddQueryLocation(aggr);
		return BindResult(std::move(error));
	}
	auto bound_function = func.functions.GetFunctionByOffset(best_function_idx.GetIndex());

	// 3. Create the BoundAggregateExpression itself.
	auto bound_aggregate = function_binder.BindAggregateFunction(bound_function, std::move(children));
	// DECIDE uses aggregate aliases for semantic transport tags (qualified
	// reducers, optimizer markers, provenance). DuckDB's generic binder does not
	// need them here, but dropping one would make the later DECIDE pass blind.
	bound_aggregate->alias = aggr.alias;
	// Note: We are NOT storing this aggregate in a BoundSelectNode. We are returning it directly.
	return BindResult(std::move(bound_aggregate));
}

static BoundAggregateExpression *GetBoundAggregate(Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		return &expr.Cast<BoundAggregateExpression>();
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		return GetBoundAggregate(*cast.child);
	}
	return nullptr;
}

BindResult DecideBinder::BindQualifiedReducer(FunctionExpression &qualified_expr, idx_t depth) {
	if (qualified_expr.children.size() < 2) {
		return BindResult(BinderException::Unsupported(
		    qualified_expr, "A qualified reducer expects one or more relations and an expression, as in "
		                    "sum(D: ...) or sum(D, T: ...)."));
	}
	vector<string> relations;
	for (idx_t i = 1; i < qualified_expr.children.size(); i++) {
		auto &qualifier = *qualified_expr.children[i];
		if (qualifier.GetExpressionClass() != ExpressionClass::COLUMN_REF ||
		    qualifier.Cast<ColumnRefExpression>().IsQualified()) {
			return BindResult(BinderException::Unsupported(
			    qualified_expr,
			    "The qualifier of a reducer must be a relation name or alias, as in sum(D: ...)."));
		}
		relations.push_back(qualifier.Cast<ColumnRefExpression>().GetColumnName());
	}
	auto relation_list = StringUtil::Join(relations, ", ");

	if (!qualifier_context) {
		return BindResult(BinderException::Unsupported(
		    qualified_expr, "A relation-qualified reducer is only allowed inside a DECIDE clause."));
	}
	auto &ctx = *qualifier_context;
	KeyColumns key;
	for (auto &relation : relations) {
		ErrorData binding_error;
		auto binding = binder.bind_context.GetBinding(relation, binding_error);
		if (!binding || binding->index == ctx.decide_index) {
			return BindResult(BinderException::Unsupported(
			    qualified_expr, StringUtil::Format(
			                        "Relation '%s' is not in the FROM clause, so a reducer cannot be qualified by it.",
			                        relation)));
		}
		AddRelationColumns(*binding, key);
	}
	// `SUM(D, T: e)` is `sum(per D, T: e)` (language/guide.md §5.1): the same exact scope, so the same
	// classes and the same check that the key determines the body.
	auto scope_idx =
	    FindOrCreateScope(FindOrCreateDecideKey(KeyElements(key), relation_list, *ctx.keys), *ctx.entity_scopes);

	auto aggregate_result = BindExpression(qualified_expr.children[0], depth);
	if (aggregate_result.HasError()) {
		return aggregate_result;
	}
	auto aggregate_expr = std::move(aggregate_result.expression);
	auto *aggregate = GetBoundAggregate(*aggregate_expr);
	if (!aggregate) {
		return BindResult(BinderException::Unsupported(
		    qualified_expr, StringUtil::Format(
		                        "A relation qualifier is only allowed on SUM, AVG, MIN or MAX; write %s(...) without "
		                        "the '%s:' qualifier.",
		                        StringUtil::Upper(relation_list), relation_list)));
	}
	auto agg_name = StringUtil::Lower(aggregate->function.name);
	if (!IsDecideAggregateName(agg_name)) {
		return BindResult(BinderException::Unsupported(
		    qualified_expr,
		    StringUtil::Format("'%s' cannot be qualified by a relation; only SUM, AVG, MIN and MAX can.",
		                       StringUtil::Upper(agg_name))));
	}
	AddDecideTag(aggregate->alias, MakeQualifiedReducerTag(scope_idx));
	AddDecideTag(aggregate->alias, COLON_FORM_TAG);
	return BindResult(std::move(aggregate_expr));
}

//! The names a key element may not take: every declared decision, as the user may spell it.
static case_insensitive_set_t DecisionNames(const case_insensitive_map_t<idx_t> &variables) {
	case_insensitive_set_t names;
	for (auto &variable : variables) {
		names.insert(variable.first);
	}
	return names;
}

BindResult DecideBinder::BindScopedReducer(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	auto &reducer = expr_ptr->Cast<FunctionExpression>();
	// Peel `[when c] [per K]:` off the argument; the reducer itself reduces the body.
	auto argument = std::move(reducer.children[0]);
	unique_ptr<ParsedExpression> per;
	if (IsDecideMarker(*argument, SCOPE_PER_ROW_TAG)) {
		// `per row`, the default written out: one value per row, as without it.
		auto body = std::move(argument->Cast<FunctionExpression>().children[0]);
		argument = std::move(body);
	} else if (IsDecideMarker(*argument, SCOPE_PER_TAG)) {
		auto body = std::move(argument->Cast<FunctionExpression>().children[0]);
		per = std::move(argument);
		argument = std::move(body);
	}
	unique_ptr<ParsedExpression> condition;
	if (IsDecideMarker(*argument, SCOPE_WHEN_TAG)) {
		auto &when = argument->Cast<FunctionExpression>();
		condition = std::move(when.children[1]);
		auto body = std::move(when.children[0]);
		argument = std::move(body);
	}
	reducer.children[0] = std::move(argument);

	auto result = BindFunction(expr_ptr, depth);
	if (result.HasError()) {
		return result;
	}
	auto *aggregate = GetBoundAggregate(*result.expression);
	if (!aggregate) {
		throw InternalException("DECIDE: a scoped reducer did not bind to an aggregate");
	}
	if (condition) {
		// The condition takes data only (ValidateDecideScopeSpellings), so it binds as plain
		// SQL: a compound condition needs no parentheses.
		ErrorData condition_error;
		binding_when_condition = true;
		try {
			BindChild(condition, depth, condition_error);
		} catch (...) {
			binding_when_condition = false;
			throw;
		}
		binding_when_condition = false;
		if (condition_error.HasError()) {
			return BindResult(std::move(condition_error));
		}
		aggregate->filter = BoundCastExpression::AddCastToType(
		    context, std::move(BoundExpression::GetExpression(*condition)), LogicalType::BOOLEAN);
	}
	if (per) {
		if (!qualifier_context) {
			throw InternalException("DECIDE: a reducer's per needs the clause's key scopes");
		}
		auto &marker = per->Cast<FunctionExpression>();
		auto scope_idx =
		    FindOrCreateKeyScope(binder.bind_context, marker, DecisionNames(variables), DecideKeyToString(marker),
		                         *qualifier_context->keys, *qualifier_context->entity_scopes);
		AddDecideTag(aggregate->alias, MakeQualifiedReducerTag(scope_idx));
	}
	return result;
}

BindResult DecideBinder::BindReducerBy(FunctionExpression &by, idx_t depth) {
	// Read before the key elements bind in place.
	auto key_text = DecideKeyToString(by);
	auto result = BindExpression(by.children[0], depth);
	if (result.HasError()) {
		return result;
	}
	auto *aggregate = GetBoundAggregate(*result.expression);
	if (!aggregate) {
		throw InternalException("DECIDE: a reducer with by did not bind to an aggregate");
	}
	if (by.children.size() == 1) {
		return result; // `by ()`: every kept row, the default
	}
	if (!qualifier_context || !qualifier_context->keys) {
		throw InternalException("DECIDE: a reducer's by needs the clause's keys");
	}
	// Each element is a data expression, or a relation standing for its stored columns.
	vector<DecideKeyElement> key;
	for (idx_t i = 1; i < by.children.size(); i++) {
		auto &element = by.children[i];
		if (element->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto relation = ResolveRelationName(binder.bind_context, element->Cast<ColumnRefExpression>());
			if (relation) {
				for (auto &column : BindRelationColumns(*relation)) {
					key.push_back(std::move(column));
				}
				continue;
			}
		}
		DecideKeyElement key_element;
		key_element.name = element->ToString();
		ErrorData key_error;
		binding_when_condition = true;
		try {
			BindChild(element, depth, key_error);
		} catch (...) {
			binding_when_condition = false;
			throw;
		}
		binding_when_condition = false;
		if (key_error.HasError()) {
			return BindResult(std::move(key_error));
		}
		key_element.expression = std::move(BoundExpression::GetExpression(*element));
		key.push_back(std::move(key_element));
	}
	// The key as the user wrote it, when the parser kept it.
	string written_key;
	if (qualifier_context->source_text &&
	    qualifier_context->source_text->Cut(by.GetQueryLocation(), DecideSpanRole::BY, written_key)) {
		key_text = std::move(written_key);
	}
	auto group_key = FindOrCreateDecideKey(std::move(key), key_text, *qualifier_context->keys);
	if (group_key != DConstants::INVALID_INDEX) {
		(*qualifier_context->keys)[group_key].written_as_by = true;
		AddDecideTag(aggregate->alias, MakeGroupKeyTag(group_key));
	}
	return result;
}

BindResult DecideBinder::BindFunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &function = expr.Cast<FunctionExpression>();
    if (function.is_operator && function.function_name == QUALIFIED_REDUCER_TAG) {
        return BindQualifiedReducer(function, depth);
    }
	// `by (key)` and a scope in front of a reducer's argument. ValidateDecideScopeSpellings
	// has already refused both on anything but SUM, AVG, MIN and MAX.
	if (function.is_operator && function.function_name == REDUCER_BY_TAG) {
		return BindReducerBy(function, depth);
	}
	if (!function.is_operator && !function.children.empty() && IsDecideScopeMarker(*function.children[0])) {
		return BindScopedReducer(expr_ptr, depth);
	}
	// NORM is DECIDE syntax, not a catalog function. Bind it as a deliberately
	// inert SUM aggregate marker; DecideOptimizer owns the mathematical rewrite
	// and replaces this marker before it can reach physical planning. Keeping an
	// aggregate-shaped marker is important: a `by (G)` after it uses the same
	// bound-tree contract as on any other reducer.
	if (!function.is_operator && StringUtil::CIEquals(function.function_name, "norm")) {
		if (function.children.size() < 2 || function.children.size() > 3) {
			return BindResult(BinderException::Unsupported(
                function, "norm(expr, p) takes an expression, an order (1, 2, 0, or 'inf'), and optional M for L0."));
		}
		if (function.children[1]->GetExpressionClass() != ExpressionClass::CONSTANT) {
            return BindResult(BinderException::Unsupported(
                function, "The order p in norm(expr, p) must be a constant (1, 2, 0, or 'inf')."));
        }
        auto &order_value = function.children[1]->Cast<ConstantExpression>().value;
        string payload;
        if (order_value.type().id() == LogicalTypeId::VARCHAR) {
            auto order = StringUtil::Lower(StringValue::Get(order_value));
            if (order != "inf" && order != "infinity" && order != "max") {
                return BindResult(BinderException::Unsupported(function,
                    StringUtil::Format("Unsupported norm order '%s'. Use 0, 1, 2, or 'inf'.", order)));
            }
            if (function.children.size() != 2) {
                return BindResult(BinderException::Unsupported(function, "norm(expr, 'inf') does not take M."));
            }
            payload = "inf";
        } else if (order_value.type().IsNumeric()) {
            double order = order_value.GetValue<double>();
            if (order == 1.0) {
                if (function.children.size() != 2) return BindResult(BinderException::Unsupported(function, "Only norm(expr, 0, M) accepts M."));
                payload = "1";
            } else if (order == 2.0) {
                if (function.children.size() != 2) return BindResult(BinderException::Unsupported(function, "Only norm(expr, 0, M) accepts M."));
                payload = "2";
            } else if (order == 0.0) {
                if (function.children.size() == 2) {
                    payload = "0_auto";
                } else {
                    if (function.children[2]->GetExpressionClass() != ExpressionClass::CONSTANT) {
                        return BindResult(BinderException::Unsupported(function,
                            "norm(expr, 0, M): the bound M must be a constant. Omit it to infer M from the data, or pass a positive literal."));
                    }
                    auto m = function.children[2]->Cast<ConstantExpression>().value.GetValue<double>();
                    if (!(m > 0.0)) {
                        return BindResult(BinderException::Unsupported(function,
                            "The L0 bound M in norm(expr, 0, M) must be a positive constant."));
                    }
                    payload = "0_" + StringUtil::Format("%.17g", m);
                }
            } else {
                return BindResult(BinderException::Unsupported(function,
                    StringUtil::Format("Unsupported norm order %g. Supported: 0, 1, 2, or 'inf'.", order)));
            }
        } else {
            return BindResult(BinderException::Unsupported(function,
                "The order p in norm(expr, p) must be 0, 1, 2, or 'inf'."));
        }
        vector<unique_ptr<ParsedExpression>> marker_children;
        marker_children.push_back(std::move(function.children[0]));
        auto marker = make_uniq<FunctionExpression>("sum", std::move(marker_children));
        marker->alias = function.alias;
        AddDecideTag(marker->alias, string(NORM_MARKER_TAG_PREFIX) + payload + "__");
        expr_ptr = std::move(marker);
        return BindFunction(expr_ptr, depth);
	}
	// Check if this is an aggregate function
	QueryErrorContext error_context(expr_ptr->GetQueryLocation());
    auto &catalog_entry = *GetCatalogEntry(CatalogType::SCALAR_FUNCTION_ENTRY, function.catalog, function.schema, function.function_name, OnEntryNotFound::THROW_EXCEPTION, error_context);
    
    if (catalog_entry.type == CatalogType::AGGREGATE_FUNCTION_ENTRY) {
        // It's an aggregate function, bind it using our custom aggregate logic
        return BindAggregate(function, catalog_entry.Cast<AggregateFunctionCatalogEntry>(), depth);
    }
	// It's a scalar function, bind it normally. A NULL argument makes it NULL, and DuckDB
	// folds it to that constant: the decisions it read are gone, so it is refused here,
	// where the SQL is still at hand.
	bool reads_decision = ExpressionContainsDecideVariable(function, variables);
	auto text = reads_decision ? function.ToString() : string();
	auto result = ExpressionBinder::BindExpression(expr_ptr, depth);
	if (reads_decision && !result.HasError() &&
	    result.expression->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
	    result.expression->Cast<BoundConstantExpression>().value.IsNull()) {
		throw BinderException(function,
		                      "DECIDE: '%s' is NULL whatever the decisions are; replace the NULL with a value, as in "
		                      "COALESCE(column, 0)",
		                      text);
	}
	return result;
}

BindResult DecideBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
    // Read the location before dispatching: BindFunction rewrites `expr_ptr` in place
    // for the norm marker, so the node it names afterwards is not the one the user wrote.
    auto location = expr_ptr->GetQueryLocation();
    return PreserveQueryLocation(location, BindExpressionInternal(expr_ptr, depth, root_expression));
}

BindResult DecideBinder::BindExpressionInternal(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                                bool root_expression) {
    if (depth > 0) {
        return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
    }
    auto &expr = *expr_ptr;
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::FUNCTION:
        return BindFunction(expr_ptr, depth);
    case ExpressionClass::SUBQUERY: {
        auto &subquery = expr.Cast<SubqueryExpression>();
        if (subquery.subquery_type != SubqueryType::SCALAR) {
             return BindResult(BinderException::Unsupported(expr, "Only scalar subqueries are supported in DECIDE"));
        }
        if (ExpressionContainsDecideVariable(expr, variables)) {
            return BindResult(BinderException::Unsupported(expr,
                "Subqueries in DECIDE clauses cannot reference DECIDE variables."));
        }
        // Standard binding handles both correlated and uncorrelated scalar subqueries.
        // Uncorrelated: PlanSubqueries evaluates them as cross-joined scalars.
        // Correlated: PlanSubqueries decorrelates them into joins, producing
        //             per-row values that the DECIDE operator evaluates normally.
        return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
    }
    default:
        return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
    }
}



} // namespace duckdb
