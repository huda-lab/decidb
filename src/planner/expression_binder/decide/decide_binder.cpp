#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/decide_frame_expression.hpp"
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
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/table_binding.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/execution/expression_executor.hpp"
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

void RefuseDecideTagInName(const ParsedExpression &at, const string &name) {
	if (DecideNameContainsTag(name)) {
		throw BinderException(at, "'%s' is not allowed as a name in DECIDE", name);
	}
}

static void StripTagsInPlace(ParsedExpression &expr) {
	expr.SetAlias(StripDecideTags(expr.GetAlias()));
	ParsedExpressionIterator::EnumerateChildren(expr, [&](ParsedExpression &child) { StripTagsInPlace(child); });
}

//! `expr` in DeciDB's own spelling: its ToString, without the binders' tags its arguments' aliases carry.
static string DecideSpelling(const ParsedExpression &expr) {
	auto copy = expr.Copy();
	StripTagsInPlace(*copy);
	return copy->ToString();
}

string DecideWrittenSql(const ParsedExpression &expr, const DecideSourceText *text) {
	string written;
	if (text && text->Cut(expr.GetQueryLocation(), DecideSpanRole::EXPR, written)) {
		return written;
	}
	// A node with no span reprints.
	return DecideSpelling(expr);
}

//! ParsedExpressionIterator::EnumerateChildren, plus the body (SELECT list, WHERE,
//! FROM, ...) of a subquery, which EnumerateChildren does not enter. A DECIDE nested in
//! the subquery keeps its clauses to itself: its own binder checks them.
static void EnumerateChildrenAndSubqueries(const ParsedExpression &expr,
                                           const std::function<void(const ParsedExpression &child)> &callback) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery_expr = expr.Cast<const SubqueryExpression>();
		// const_cast: EnumerateQueryNodeChildren requires non-const but we only read
		ParsedExpressionIterator::EnumerateQueryNodeChildren(
		    const_cast<QueryNode &>(*subquery_expr.subquery->node),
		    [&](unique_ptr<ParsedExpression> &child) { callback(*child); }, [&](TableRef &ref) {},
		    /*decide_clauses=*/false);
	}
	ParsedExpressionIterator::EnumerateChildren(expr, callback);
}

bool ExpressionContainsDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	bool found = IsVariableExpression(expr, variables);
	EnumerateChildrenAndSubqueries(expr, [&](const ParsedExpression &child) {
		found = found || ExpressionContainsDecideVariable(child, variables);
	});
	return found;
}

void ValidateDecideNoExplicitDecisionCasts(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                           const DecideSourceText *text) {
	if (expr.GetExpressionClass() == ExpressionClass::CAST) {
		auto &cast = expr.Cast<const CastExpression>();
		// Transformer::TransformTypeCast stamps CAST, TRY_CAST and :: with their
		// source location. CastExpression nodes synthesized by the parser itself
		// (for example for a boolean test) have no location and are not user casts.
		if (cast.GetQueryLocation().IsValid() && ExpressionContainsDecideVariable(*cast.child, variables)) {
			throw BinderException(cast, "'%s' casts a decision", DecideWrittenSql(cast, text));
		}
	}

	// Subquery bodies too, so a correlated decision reference cannot hide an
	// explicit cast inside one.
	EnumerateChildrenAndSubqueries(
	    expr, [&](const ParsedExpression &child) { ValidateDecideNoExplicitDecisionCasts(child, variables, text); });
}

//===--------------------------------------------------------------------===//
// Where when, per and by may stand
//===--------------------------------------------------------------------===//

//! Refuses every `when`, `per` and `by` inside `expr`, subquery bodies included, as not bound
//! there: a `when` condition, a key, a `by` key and an ordinary SQL subquery written in
//! the clause (which is lexed with the clause's keywords) take data only.
static void ValidateNoScopeSpelling(const ParsedExpression &expr, const DecideSourceText *text) {
	if (IsDecideScope(expr)) {
		// The outer keyword names the scope: its `per`, else its `when`.
		const char *spelling = "when";
		switch (ReadDecideScope(expr).per) {
		case DecidePer::KEY:
			spelling = "per";
			break;
		case DecidePer::ALL:
			spelling = "per ()";
			break;
		case DecidePer::ROW:
			spelling = "per row";
			break;
		default:
			break;
		}
		throw NotImplementedException(expr, "'%s' inside a subquery is not supported", spelling);
	}
	if (IsDecideBy(expr)) {
		throw NotImplementedException(expr, "'%s' inside a subquery is not supported", "by");
	}
	if (IsDecideRelationPrefix(expr)) {
		throw NotImplementedException(expr, "'%s' inside a subquery is not supported", DecideWrittenSql(expr, text));
	}
	EnumerateChildrenAndSubqueries(expr, [&](const ParsedExpression &child) { ValidateNoScopeSpelling(child, text); });
}

//! The first reference to a decision in `expr`, subquery bodies included, or null: `when`, `per` and `by` take data.
static optional_ptr<const ParsedExpression> FindDecision(const ParsedExpression &expr,
                                                         const case_insensitive_map_t<idx_t> &variables) {
	if (IsVariableExpression(expr, variables)) {
		return &expr;
	}
	optional_ptr<const ParsedExpression> found;
	EnumerateChildrenAndSubqueries(expr, [&](const ParsedExpression &child) {
		if (!found) {
			found = FindDecision(child, variables);
		}
	});
	return found;
}

//! Refuses `decision`, a reference to a decision, where only data may stand: `where` names the position
//! (DecisionInDataMessage).
[[noreturn]] static void ThrowDecisionInData(const ParsedExpression &decision, const DecideSourceText *text,
                                             const char *where) {
	throw BinderException(decision, "%s", DecisionInDataMessage(DecideWrittenSql(decision, text), where));
}

//! What a `when` condition, a key, a `by` key and a frame's order key cannot hold: they select the rows a scope reads,
//! so they read none themselves.
static const char *const WINDOW_IN_DATA_MESSAGE =
    "a window function is not allowed in when, per, by or a frame's order key";
static const char *const REDUCER_IN_DATA_MESSAGE = "a reducer is not allowed in when, per, by or a frame's order key";

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

//! The first node of class `expression_class` in `expr` outside its subqueries, or null.
static optional_ptr<const ParsedExpression> FindExpressionOfClass(const ParsedExpression &expr,
                                                                  ExpressionClass expression_class) {
	return FindNode(expr, [&](const ParsedExpression &node) { return node.GetExpressionClass() == expression_class; });
}

//! A frame selector reads rows by a timeline the scope itself chooses, so none stands in
//! a `when`, a key or an order key (D80).
static void ValidateNoFrame(const ParsedExpression &expr) {
	auto frame = FindExpressionOfClass(expr, ExpressionClass::DECIDE_FRAME);
	if (frame) {
		throw BinderException(*frame, "a frame selector is not allowed in when, per, by, within or an order key");
	}
}

//! A `when` condition, a key element or a `by` key: data only, no `when`/`per`/`by`, and
//! no window function, reducer or frame selector, which read rows the scope has not chosen yet.
static void ValidateScopeData(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                              const DecideSourceText *text) {
	auto decision = FindDecision(expr, variables);
	if (decision) {
		ThrowDecisionInData(*decision, text, "when, per and by");
	}
	auto window = FindNode(
	    expr, [](const ParsedExpression &node) { return node.GetExpressionClass() == ExpressionClass::WINDOW; });
	if (window) {
		throw BinderException(*window, WINDOW_IN_DATA_MESSAGE);
	}
	auto reducer = FindNode(expr, [](const ParsedExpression &node) { return TakesReducerBy(node); });
	if (reducer) {
		throw BinderException(*reducer, REDUCER_IN_DATA_MESSAGE);
	}
	ValidateNoFrame(expr);
	ValidateNoScopeSpelling(expr, text);
}

//! A frame's order key or `within` key: data only, with no frame or window inside.
static void ValidateFrameKey(const ParsedExpression &key, const case_insensitive_map_t<idx_t> &variables,
                             const DecideSourceText *text) {
	auto decision = FindDecision(key, variables);
	if (decision) {
		ThrowDecisionInData(*decision, text, "a frame's order key and within");
	}
	ValidateNoFrame(key);
	auto window = FindExpressionOfClass(key, ExpressionClass::WINDOW);
	if (window) {
		throw BinderException(*window, WINDOW_IN_DATA_MESSAGE);
	}
	std::function<void(const ParsedExpression &)> refuse_reducer = [&](const ParsedExpression &node) {
		if (TakesReducerBy(UnwrapReducerMarkers(node))) {
			throw BinderException(node, REDUCER_IN_DATA_MESSAGE);
		}
		ParsedExpressionIterator::EnumerateChildren(node, refuse_reducer);
	};
	refuse_reducer(key);
	ValidateNoScopeSpelling(key, text);
}

//! A frame's payload and fallback are read at one row, so neither holds a reducer or
//! another frame (D80).
static void ValidateFramePayload(const ParsedExpression &expr, const DecideFrameExpression &frame,
                                 const DecideSourceText *text) {
	if (expr.GetExpressionClass() == ExpressionClass::DECIDE_FRAME) {
		throw BinderException(expr, "a frame selector inside '%s' is not allowed", DecideWrittenSql(frame, text));
	}
	if (TakesReducerBy(UnwrapReducerMarkers(expr))) {
		throw BinderException(expr, "a reducer inside '%s' is not allowed", DecideWrittenSql(frame, text));
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { ValidateFramePayload(child, frame, text); });
}

static void ValidateFrame(const DecideFrameExpression &frame, const case_insensitive_map_t<idx_t> &variables,
                          const DecideSourceText *text) {
	ValidateFrameKey(*frame.order, variables, text);
	for (auto &key : frame.within) {
		ValidateFrameKey(*key, variables, text);
	}
	for (auto *value : {frame.payload.get(), frame.fallback.get()}) {
		if (value) {
			ValidateFramePayload(*value, frame, text);
			ValidateNoScopeSpelling(*value, text);
		}
	}
	// A frame reads a decision through its payload: the layers below see a frame's payload,
	// not its fallback, so one whose fallback alone reads a decision is not formulated yet.
	if (frame.fallback && ExpressionContainsDecideVariable(*frame.fallback, variables) &&
	    !ExpressionContainsDecideVariable(*frame.payload, variables)) {
		throw NotImplementedException(*frame.fallback,
		                              "else '%s' reads a decision while the payload reads none, which is not supported",
		                              DecideWrittenSql(*frame.fallback, text));
	}
}

//! `[when c] [per K]:` off a clause or a reducer's argument: the key elements and the
//! condition are validated as data, and the body is returned.
static const ParsedExpression &PeelScopeSpelling(const ParsedExpression &expr,
                                                 const case_insensitive_map_t<idx_t> &variables,
                                                 const DecideSourceText *text) {
	if (!IsDecideScope(expr)) {
		return expr;
	}
	auto scope = ReadDecideScope(expr);
	for (auto &element : scope.key) {
		ValidateScopeData(*element, variables, text);
	}
	if (scope.when) {
		ValidateScopeData(**scope.when, variables, text);
	}
	return **scope.body;
}

namespace {
//! Where in a clause a node stands, for the placement rules.
struct ScopePlacement {
	const case_insensitive_map_t<idx_t> &variables;
	//! The query's text, which a message quotes.
	const DecideSourceText *text;
	//! Inside a reducer's argument.
	bool in_reducer = false;
};
} // namespace

static void ValidateScopePlacement(const ParsedExpression &expr, ScopePlacement placement) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		ValidateNoScopeSpelling(expr, placement.text);
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::DECIDE_FRAME) {
		ValidateFrame(expr.Cast<DecideFrameExpression>(), placement.variables, placement.text);
		return;
	}
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		ParsedExpressionIterator::EnumerateChildren(
		    expr, [&](const ParsedExpression &child) { ValidateScopePlacement(child, placement); });
		return;
	}
	if (IsDecideBy(expr)) {
		auto by = ReadDecideBy(expr);
		if (!TakesReducerBy(UnwrapReducerMarkers(**by.call))) {
			throw BinderException(expr, "by (...) does not follow a reducer");
		}
		for (auto &element : by.key) {
			ValidateScopeData(*element, placement.variables, placement.text);
		}
		ValidateScopePlacement(**by.call, placement);
		return;
	}
	auto &func = expr.Cast<const FunctionExpression>();
	bool scoped_argument = IsDecideScopedCall(expr);
	if (scoped_argument && !IsDecideAggregateName(func.function_name)) {
		throw BinderException(expr, "when or per inside '%s', which is not a reducer",
		                      DecideWrittenSql(expr, placement.text));
	}
	bool reducer = TakesReducerBy(func);
	// A reducer over decisions may not stand inside a norm (D80), which has no tree lowering yet. Under an ABS, a
	// POWER or a product with a decision it stays at its written position and the tree lowers it beneath its reader.
	if (!func.is_operator && !func.children.empty() && ContainsDecideAggregate(*func.children[0]) &&
	    ExpressionContainsDecideVariable(*func.children[0], placement.variables) &&
	    StringUtil::CIEquals(func.function_name, "norm")) {
		throw BinderException(expr, "a reducer inside norm is not allowed");
	}
	// A reducer nests in another to any depth (language/guide.md §5.5), a norm aside (D80). One over data alone is a
	// value of its path's group, which the bound-side rewrite computes (RewriteRhsAggregates).
	if (reducer && placement.in_reducer && StringUtil::CIEquals(func.function_name, "norm")) {
		throw BinderException(expr, "norm inside a reducer is not allowed");
	}
	auto inner = placement;
	if (reducer) {
		inner.in_reducer = true;
	}
	for (idx_t i = 0; i < func.children.size(); i++) {
		auto &child = i == 0 && scoped_argument
		                  ? PeelScopeSpelling(*func.children[0], placement.variables, placement.text)
		                  : *func.children[i];
		ValidateScopePlacement(child, reducer ? inner : placement);
	}
	if (func.filter) {
		ValidateNoScopeSpelling(*func.filter, placement.text);
	}
}

void ValidateDecideScopeSpellings(const ParsedExpression &clause, const case_insensitive_map_t<idx_t> &variables,
                                  const DecideSourceText *text) {
	auto &body = PeelScopeSpelling(clause, variables, text);
	// Every clause follows the scope rules (D30).
	ValidateScopePlacement(body, ScopePlacement {variables, text});
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
	//! The column's own name, which its bound reference carries; `name` is the SQL spelling beside it.
	string column_name;
	string name;
	DecideKeyNullMode null_mode;
};
using KeyColumns = vector<KeyColumn>;

//! Column `column_index` of `binding` as a key column a relation element stands for,
//! registered with the scan.
static KeyColumn MakeKeyColumn(Binding &binding, column_t column_index) {
	return KeyColumn {RegisterKeyColumn(binding, column_index), binding.types[column_index],
	                  binding.names[column_index],
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

//! Adds column `column_index` of `binding`, which `element` names, to `columns`. Its name is the text the query wrote
//! for `element`.
static void AddKeyColumn(const ColumnRefExpression &element, Binding &binding, column_t column_index,
                         KeyColumns &columns, const DecideSourceText *text) {
	if (!IsStoredColumn(binding, column_index)) {
		throw BinderException(element, "'%s' is not a stored column", DecideWrittenSql(element, text));
	}
	auto column = MakeKeyColumn(binding, column_index);
	column.name = DecideWrittenSql(element, text);
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
static optional_ptr<Binding> UnqualifiedColumnBinding(BindContext &bind_context, const ColumnRefExpression &element,
                                                      const DecideSourceText *text) {
	auto &name = element.GetColumnName();
	auto using_set = bind_context.GetUsingBinding(name);
	if (!using_set) {
		return bind_context.GetMatchingBinding(name);
	}
	if (!using_set->primary_binding.IsSet()) {
		throw BinderException(element, "'%s' is merged by FULL OUTER JOIN USING", DecideWrittenSql(element, text));
	}
	ErrorData error;
	return bind_context.GetBinding(using_set->primary_binding, error);
}

string OuterQueryColumnMessage(const string &column_name) {
	return StringUtil::Format("'%s' refers to an outer query", column_name);
}

string DecisionInDataMessage(const string &decision, const string &where) {
	return StringUtil::Format("'%s' is a decision, which %s cannot read", decision, where);
}

string ComparisonUsedAsNumberMessage(const string &expression) {
	return StringUtil::Format("'%s' is a comparison used as a number", expression);
}

string ArgumentCountMessage(const string &call, idx_t count) {
	return StringUtil::Format("'%s' has %llu argument%s", call, count, count == 1 ? "" : "s");
}

string SubqueryNotScalarMessage() {
	return "a subquery in a DECIDE clause is not scalar";
}

string SubqueryReadsDecisionMessage() {
	return "a subquery in a DECIDE clause reads a decision";
}

//! A frame's `else` that is not a number.
static string NotANumberMessage(const string &fallback_text) {
	return StringUtil::Format("else '%s' is not a number", fallback_text);
}

void ThrowIfOuterColumn(Binder &binder, const ColumnRefExpression &column) {
	for (auto &outer : binder.GetActiveBinders()) {
		auto probe = column.Copy();
		ErrorData error;
		// The base lookup: an outer DECIDE binder would come back here.
		if (outer.get().ExpressionBinder::QualifyColumnName(probe->Cast<ColumnRefExpression>(), error)) {
			throw BinderException(column, "%s", OuterQueryColumnMessage(column.ToString()));
		}
	}
}

unique_ptr<ParsedExpression> DecideBinder::QualifyColumnName(ColumnRefExpression &col_ref, ErrorData &error) {
	auto qualified = ExpressionBinder::QualifyColumnName(col_ref, error);
	if (!qualified) {
		ThrowIfOuterColumn(binder, col_ref);
	} else {
		RefuseDecideTagInName(col_ref, col_ref.column_names.back());
	}
	return qualified;
}

//! Resolves one element of a `per` key into `columns`, against the FROM clause alone and
//! the way SQL resolves the same name: a column (`c`, `R.c`, `S.R.c`, `C.S.R.c`), else a
//! relation (`R`, `S.R`, `C.S.R`), standing for every column it stores.
static void ResolveKeyElement(Binder &binder, const ColumnRefExpression &element,
                              const case_insensitive_set_t &decision_names, KeyColumns &columns,
                              const DecideSourceText *text) {
	auto &bind_context = binder.bind_context;
	auto &names = element.column_names;
	if (names.size() == 1 && decision_names.count(names[0])) {
		ThrowDecisionInData(element, text, "when, per and by");
	}
	ErrorData error;
	optional_ptr<Binding> binding;
	if (names.size() == 1) {
		binding = UnqualifiedColumnBinding(bind_context, element, text);
	} else if (names.size() <= 4) {
		binding =
		    bind_context.GetBinding(RelationAlias(vector<string>(names.begin(), names.end() - 1)), names.back(), error);
	}
	if (binding) {
		RefuseDecideTagInName(element, names.back());
		AddKeyColumn(element, *binding, binding->GetBindingIndex(names.back()), columns, text);
		return;
	}
	auto relation = names.size() <= 3 ? bind_context.GetBinding(RelationAlias(names), error) : nullptr;
	if (!relation) {
		ThrowIfOuterColumn(binder, element);
		throw BinderException(element, "'%s' is not a column or a relation of the FROM clause",
		                      DecideWrittenSql(element, text));
	}
	AddRelationColumns(*relation, columns);
}

//! The columns of the key `key` lists (none for `()`), each element resolved by ResolveKeyElement.
static KeyColumns ResolveKey(Binder &binder, const DecideKeyView &key, const case_insensitive_set_t &decision_names,
                             const DecideSourceText *text) {
	KeyColumns columns;
	for (auto &element : key) {
		ResolveKeyElement(binder, element->Cast<ColumnRefExpression>(), decision_names, columns, text);
	}
	return columns;
}

//! `columns` as key elements, for FindOrCreateDecideKey to order and de-duplicate.
static vector<DecideKeyElement> KeyElements(ClientContext &context, const KeyColumns &columns) {
	vector<DecideKeyElement> elements;
	for (auto &column : columns) {
		elements.push_back(DecideKeyElement::Bind(
		    context, make_uniq<BoundColumnRefExpression>(column.column_name, column.type, column.binding), column.name,
		    column.null_mode));
	}
	return elements;
}

//! The scope of the decisions and reducers keyed by `key`: one per key.
static idx_t FindOrCreateScope(idx_t key, vector<KeyedScopeInfo> &keyed_scopes) {
	for (idx_t scope_idx = 0; scope_idx < keyed_scopes.size(); scope_idx++) {
		if (keyed_scopes[scope_idx].key == key) {
			return scope_idx;
		}
	}
	KeyedScopeInfo scope_info;
	scope_info.key = key;
	keyed_scopes.push_back(std::move(scope_info));
	return keyed_scopes.size() - 1;
}

idx_t FindOrCreateKeyScope(ClientContext &context, Binder &binder, const DecideKeyView &key,
                           const case_insensitive_set_t &decision_names, const string &key_text,
                           const DecideSourceText *text, vector<DecideKey> &keys,
                           vector<KeyedScopeInfo> &keyed_scopes) {
	return FindOrCreateScope(BindKey(context, binder, key, decision_names, key_text, text, keys), keyed_scopes);
}

idx_t BindKey(ClientContext &context, Binder &binder, const DecideKeyView &key,
              const case_insensitive_set_t &decision_names, const string &key_text, const DecideSourceText *text,
              vector<DecideKey> &keys) {
	return FindOrCreateDecideKey(KeyElements(context, ResolveKey(binder, key, decision_names, text)), key_text, keys);
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
static vector<DecideKeyElement> BindRelationColumns(ClientContext &context, Binding &relation) {
	KeyColumns columns;
	AddRelationColumns(relation, columns);
	return KeyElements(context, columns);
}

bool ContainsDecideAggregate(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (!func.is_operator &&
		    (IsDecideAggregateName(func.function_name) || StringUtil::CIEquals(func.function_name, "norm"))) {
			return true;
		}
		// A `by` key is not part of the value.
		if (IsDecideBy(expr)) {
			return ContainsDecideAggregate(**ReadDecideBy(expr).call);
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

DecideBinder::DecideBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables,
                           const case_insensitive_set_t &scalar_variables,
                           optional_ptr<DecideQualifierContext> qualifier_context)
    : ExpressionBinder(binder, context), variables(variables), scalar_variables(scalar_variables),
      qualifier_context(qualifier_context) {
	is_top_expression = true;
}

ErrorData DecideBinder::BindDataChild(unique_ptr<ParsedExpression> &expr, idx_t depth) {
	ErrorData error;
	binding_when_condition = true;
	try {
		BindChild(expr, depth, error);
	} catch (...) {
		binding_when_condition = false;
		throw;
	}
	binding_when_condition = false;
	return error;
}

BindResult DecideBinder::PreserveQueryLocation(optional_idx location, BindResult result) {
	// Only fill an empty location: a nested bind that already recorded a more precise
	// node should keep it rather than be widened to its parent's span.
	if (!result.HasError() && !result.expression->GetQueryLocation().IsValid()) {
		result.expression->SetQueryLocation(location);
	}
	return result;
}

BindResult DecideBinder::ThrowClauseError(BindResult result, bool root_expression) {
	if (root_expression && result.HasError()) {
		result.error.Throw();
	}
	return result;
}

bool DecideBinder::IsScalarDecideVariable(const ParsedExpression &expr) const {
	// `expr` is a decision reference (IsVariableExpression): a bare column name, since the declarations binder
	// refuses `D.x`.
	return scalar_variables.count(expr.Cast<const ColumnRefExpression>().GetColumnName()) > 0;
}

bool DecideBinder::IsDataOperand(const ParsedExpression &expr) const {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::FUNCTION:
	case ExpressionClass::SUBQUERY:
	case ExpressionClass::WINDOW:
		return false;
	default:
		// A frame selector over data is a value read at another row, which only the DECIDE binder reads.
		return !ExpressionContainsDecideVariable(expr, variables) &&
		       !FindExpressionOfClass(expr, ExpressionClass::DECIDE_FRAME);
	}
}

//! Whether `expr`, or a subquery body in it, names a decision that is not query-wide.
bool DecideBinder::ReadsRowDecision(const ParsedExpression &expr) const {
	bool reads = IsVariableExpression(expr, variables) && !IsScalarDecideVariable(expr);
	EnumerateChildrenAndSubqueries(expr, [&](const ParsedExpression &child) { reads |= ReadsRowDecision(child); });
	return reads;
}

bool DecideBinder::IsRowInvariantExpression(const ParsedExpression &expr) const {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		// A subquery's body is a query of its own, whose tables' columns are not this row's; it varies with the row
		// only through the per-row decisions it names.
		return !ReadsRowDecision(expr);
	}
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		if (!IsVariableExpression(expr, variables)) {
			return false; // a plain data column varies per row
		}
		return IsScalarDecideVariable(expr); // row/keyed decisions vary; scalars don't
	}
	bool invariant = true;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (invariant && !IsRowInvariantExpression(child)) {
			invariant = false;
		}
	});
	return invariant;
}

BindResult DecideBinder::BindAggregate(FunctionExpression &aggr, AggregateFunctionCatalogEntry &func, idx_t depth) {
	ErrorData error;

	// DECIDE clauses only accept SUM, AVG, MIN, MAX, and COUNT as aggregates. Reject anything
	// else (STRING_AGG, BIT_AND, MEDIAN, HISTOGRAM, etc.) at the canonical DuckDB hook,
	// mirroring WhereBinder::UnsupportedAggregateMessage. `count_star` is permitted because
	// the symbolic phase synthesizes `count_star()` internally when rewriting SUM(constant).
	if (!IsDecideAggregateName(func.name) && func.name != "count" && func.name != "count_star") {
		return BindResult(NotImplementedException(aggr, "'%s' is not supported in a DECIDE clause",
		                                          DecideWrittenSql(aggr, qualifier_context->source_text)));
	}

	// A reducer takes one argument: MAX(x, n) and MIN(x, n) are DuckDB's top-n aggregates, which return a list.
	if (IsDecideAggregateName(func.name) && aggr.children.size() != 1) {
		return BindResult(BinderException(
		    aggr, "%s",
		    ArgumentCountMessage(DecideWrittenSql(aggr, qualifier_context->source_text), aggr.children.size())));
	}
	// A reducer whose body holds the same value on every row (`sum(y)` over a query-wide `y`) reads one of two ways,
	// once or once per row, which are different problems. A norm and a frame's range reduce as any reducer does.
	if (IsDecideAggregateName(func.name) && ExpressionContainsDecideVariable(*aggr.children[0], variables) &&
	    IsRowInvariantExpression(*aggr.children[0])) {
		auto text = qualifier_context->source_text;
		return BindResult(BinderException(aggr, "'%s' is a query-wide decision, so '%s' has no rows to reduce",
		                                  DecideWrittenSql(*aggr.children[0], text), DecideWrittenSql(aggr, text)));
	}

	// COUNT over a DECIDE variable is degenerate: decision variables are never null, so
	// COUNT(x) is identically the row count and does not constrain x. Reject it with a
	// semantic-specific message rather than silently letting it bind to a no-op constraint.
	if (func.name == "count") {
		for (auto &child : aggr.children) {
			if (ExpressionContainsDecideVariable(*child, variables)) {
				return BindResult(NotImplementedException(aggr, "COUNT over a decision is not supported"));
			}
		}
	}

	// No Filter/Distinc allowed for Aggregate
	if (aggr.filter || aggr.distinct) {
		return BindResult(NotImplementedException(aggr, "'%s' is not supported in a DECIDE clause",
		                                          DecideWrittenSql(aggr, qualifier_context->source_text)));
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
		// BindExpression, unlike Bind, leaves the parsed call's tags behind: the name it was written under is kept.
		if (HasDecideTag(child_expr->GetAlias(), WRITTEN_NAME_TAG_PREFIX) &&
		    !HasDecideTag(bound_child->GetAlias(), WRITTEN_NAME_TAG_PREFIX)) {
			auto tags = bound_child->GetAlias();
			AddDecideTag(tags,
			             string(WRITTEN_NAME_TAG_PREFIX) + DecideWrittenName(child_expr->GetAlias(), string()) + "__");
			bound_child->SetAlias(std::move(tags));
		}
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
	return expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE ? &expr.Cast<BoundAggregateExpression>()
	                                                                     : nullptr;
}

BindResult DecideBinder::BindQualifiedReducer(FunctionExpression &qualified_expr, idx_t depth) {
	auto &ctx = *qualifier_context;
	// The grammar makes the prefix a list of single names (makeQualifiedReducer), one or more.
	auto prefix = ReadDecideRelationPrefix(qualified_expr);
	vector<string> relations;
	for (auto &relation : prefix.relations) {
		relations.push_back(relation->Cast<ColumnRefExpression>().GetColumnName());
	}
	auto relation_list = StringUtil::Join(relations, ", ");

	KeyColumns key;
	for (idx_t i = 0; i < relations.size(); i++) {
		ErrorData binding_error;
		auto binding = binder.bind_context.GetBinding(relations[i], binding_error);
		if (!binding || binding->index == ctx.decide_index) {
			auto &qualifier = *prefix.relations[i];
			return BindResult(BinderException(qualifier, "'%s' is not a relation of the FROM clause",
			                                  DecideWrittenSql(qualifier, ctx.source_text)));
		}
		AddRelationColumns(*binding, key);
	}
	// `SUM(D, T: e)` is `sum(per D, T: e)` (language/guide.md §5.1): the same exact scope, so the same
	// classes and the same variant signature (D70).
	auto scope_idx = FindOrCreateScope(FindOrCreateDecideKey(KeyElements(context, key), relation_list, *ctx.keys),
	                                   *ctx.keyed_scopes);

	auto aggregate_result = BindExpression(*prefix.call, depth);
	if (aggregate_result.HasError()) {
		return aggregate_result;
	}
	auto aggregate_expr = std::move(aggregate_result.expression);
	auto *aggregate = GetBoundAggregate(*aggregate_expr);
	if (!aggregate || !IsDecideAggregateName(StringUtil::Lower(aggregate->function.name))) {
		return BindResult(BinderException(qualified_expr, "'%s' cannot be qualified by a relation",
		                                  DecideWrittenSql(qualified_expr, ctx.source_text)));
	}
	AddDecideTag(aggregate->alias, MakeQualifiedReducerTag(scope_idx));
	return BindResult(std::move(aggregate_expr));
}

case_insensitive_set_t DecisionNames(const case_insensitive_map_t<idx_t> &variables) {
	case_insensitive_set_t names;
	for (auto &variable : variables) {
		names.insert(variable.first);
	}
	return names;
}

BindResult DecideBinder::BindScopedReducer(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	auto &reducer = expr_ptr->Cast<FunctionExpression>();
	// Take `[when c] [per K]:` off the argument; the reducer itself reduces the body. The scope's parts stay
	// alive in the argument as it was written.
	auto scoped_argument = std::move(reducer.children[0]);
	auto scope = ReadDecideScope(*scoped_argument);
	reducer.children[0] = std::move(*scope.body);

	auto result = BindFunction(expr_ptr, depth);
	if (result.HasError()) {
		return result;
	}
	auto *aggregate = GetBoundAggregate(*result.expression);
	if (!aggregate) {
		throw InternalException("DECIDE: a scoped reducer did not bind to an aggregate");
	}
	if (scope.when) {
		// The condition takes data only (ValidateDecideScopeSpellings), so it binds as plain
		// SQL: a compound condition needs no parentheses.
		auto condition_error = BindDataChild(*scope.when, depth);
		if (condition_error.HasError()) {
			return BindResult(std::move(condition_error));
		}
		aggregate->filter = BoundCastExpression::AddCastToType(
		    context, std::move(BoundExpression::GetExpression(**scope.when)), LogicalType::BOOLEAN);
	}
	// `per row`, the default written out, is one value per row, as without it.
	if (scope.per == DecidePer::ALL || scope.per == DecidePer::KEY) {
		auto scope_idx = FindOrCreateKeyScope(context, binder, scope.key, DecisionNames(variables),
		                                      scope.key.ToString(), qualifier_context->source_text,
		                                      *qualifier_context->keys, *qualifier_context->keyed_scopes);
		AddDecideTag(aggregate->alias, MakeQualifiedReducerTag(scope_idx));
	}
	return result;
}

BindResult DecideBinder::BindReducerBy(FunctionExpression &by, idx_t depth) {
	auto parts = ReadDecideBy(by);
	// Read before the key elements bind in place.
	auto key_text = parts.key.ToString();
	auto result = BindExpression(*parts.call, depth);
	if (result.HasError()) {
		return result;
	}
	auto *aggregate = GetBoundAggregate(*result.expression);
	if (!aggregate) {
		throw InternalException("DECIDE: a reducer with by did not bind to an aggregate");
	}
	if (parts.key.empty()) {
		return result; // `by ()`: every kept row, the default
	}
	// Each element is a data expression, or a relation standing for its stored columns.
	vector<DecideKeyElement> key;
	for (auto &element : parts.key) {
		if (element->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto relation = ResolveRelationName(binder.bind_context, element->Cast<ColumnRefExpression>());
			if (relation) {
				for (auto &column : BindRelationColumns(context, *relation)) {
					key.push_back(std::move(column));
				}
				continue;
			}
		}
		auto name = DecideWrittenSql(*element, qualifier_context->source_text);
		auto key_error = BindDataChild(element, depth);
		if (key_error.HasError()) {
			return BindResult(std::move(key_error));
		}
		key.push_back(DecideKeyElement::Bind(context, std::move(BoundExpression::GetExpression(*element)),
		                                     std::move(name), DecideKeyNullMode::NO_GROUP));
	}
	// The key as the user wrote it, when the parser kept it.
	string written_key;
	if (qualifier_context->WrittenText(by, DecideSpanRole::BY, written_key)) {
		key_text = std::move(written_key);
	}
	// Every relation stores a column, so a `by` that lists anything is a key (not INVALID_INDEX).
	auto group_key = FindOrCreateDecideKey(std::move(key), key_text, *qualifier_context->keys);
	D_ASSERT(group_key != DConstants::INVALID_INDEX);
	(*qualifier_context->keys)[group_key].written_as_by = true;
	AddDecideTag(aggregate->alias, MakeGroupKeyTag(group_key));
	return result;
}

ErrorData DecideBinder::BindFrameFallback(DecideFrameExpression &frame, const string &fallback_text, idx_t depth,
                                          unique_ptr<Expression> &out) {
	if (!frame.fallback) {
		return ErrorData();
	}
	// The fallback stands in for the payload at a missing position: a number, as the
	// payload is a number to the model.
	ErrorData fallback_error;
	// Read before the fallback binds in place.
	auto fallback_location = frame.fallback->GetQueryLocation();
	BindChild(frame.fallback, depth, fallback_error);
	if (fallback_error.HasError()) {
		return fallback_error;
	}
	auto fallback = std::move(BoundExpression::GetExpression(*frame.fallback));
	auto &type = fallback->return_type;
	if (!type.IsNumeric() && type.id() != LogicalTypeId::BOOLEAN && type.id() != LogicalTypeId::SQLNULL) {
		throw BinderException(fallback_location, "%s", NotANumberMessage(fallback_text));
	}
	// A fallback that is NULL before the query runs is the default, `else NULL` (D86): a
	// missing position selects nothing.
	Value value;
	if (fallback->IsFoldable() && ExpressionExecutor::TryEvaluateScalar(context, *fallback, value) && value.IsNull()) {
		return ErrorData();
	}
	out = std::move(fallback);
	return ErrorData();
}

unique_ptr<Expression> DecideBinder::BindFrameAggregate(const string &name, unique_ptr<Expression> payload,
                                                        const DecideFrameExpression &frame, const string &alias,
                                                        ErrorData &error) {
	auto &entry = Catalog::GetEntry<AggregateFunctionCatalogEntry>(context, SYSTEM_CATALOG, DEFAULT_SCHEMA, name);
	FunctionBinder function_binder(binder);
	auto offset = function_binder.BindFunction(entry.name, entry.functions, {payload->return_type}, error);
	if (!offset.IsValid()) {
		error.AddQueryLocation(frame);
		return nullptr;
	}
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(payload));
	auto result = function_binder.BindAggregateFunction(entry.functions.GetFunctionByOffset(offset.GetIndex()),
	                                                    std::move(children));
	result->SetAlias(alias);
	return std::move(result);
}

BindResult DecideBinder::BindDecideFrame(DecideFrameExpression &frame, idx_t depth) {
	if (!qualifier_context || !qualifier_context->frames) {
		throw InternalException("DECIDE: a frame selector needs the clause's frames");
	}
	auto *source_text = qualifier_context->source_text;
	// Read before the parts bind in place. ValidateDecideScopeSpellings has already held the
	// order key and the within key to data, and the payload and fallback to one row each.
	DecideFrameSpec spec;
	spec.range = frame.IsRange();
	spec.from = frame.from;
	spec.from_k = frame.from_k;
	spec.to = frame.to;
	spec.to_k = frame.to_k;
	spec.every = frame.every;
	spec.all = frame.all;
	spec.descending = frame.descending;
	spec.cyclic = frame.cyclic;
	spec.text = DecideWrittenSql(frame, source_text);
	spec.spelling = DecideSpelling(frame);
	// Where the frame stands, when the query text holds a span for it (a table macro's tail has none).
	spec.location = source_text ? DecideSpanLocation(*source_text, frame.GetQueryLocation()) : optional_idx();
	spec.order_name = DecideWrittenSql(*frame.order, source_text);
	auto fallback_text = frame.fallback ? DecideWrittenSql(*frame.fallback, source_text) : string();
	// The fallback's own place, which its refusals point at; read before it binds in place.
	auto fallback_location = frame.fallback ? frame.fallback->GetQueryLocation() : optional_idx();

	// The payload, under the aggregate that reads it at the selected rows: the range's
	// reducer, else `first`, which reads the one row a point selects.
	unique_ptr<Expression> result;
	if (frame.IsRange()) {
		auto name = StringUtil::Lower(frame.reducer);
		if (!IsDecideAggregateName(name) && name != "count") {
			throw NotImplementedException(frame, "a range frame reducing with '%s' is not supported", frame.reducer);
		}
		spec.reducer = name;
		vector<unique_ptr<ParsedExpression>> arguments;
		arguments.push_back(std::move(frame.payload));
		unique_ptr<ParsedExpression> call = make_uniq<FunctionExpression>(name, std::move(arguments));
		call->SetQueryLocation(frame.GetQueryLocation());
		auto bound = BindFunction(call, depth);
		if (bound.HasError()) {
			return bound;
		}
		result = std::move(bound.expression);
		// A missing position's `else` is one of the values the reducer reads, so payload and fallback take a type that
		// holds both, as a point's do: `else 0.5` over a whole-numbered payload sums 0.5 there. The reducer is bound
		// again over it.
		unique_ptr<Expression> fallback;
		auto fallback_error = BindFrameFallback(frame, fallback_text, depth, fallback);
		if (fallback_error.HasError()) {
			return BindResult(std::move(fallback_error));
		}
		if (fallback) {
			auto *reducer = GetBoundAggregate(*result);
			if (!reducer || reducer->children.empty()) {
				throw InternalException("DECIDE: a range frame did not bind to an aggregate over its payload");
			}
			if (reducer->children.size() > 1) {
				// A collated MIN or MAX compares by a key of its value (BindMinMax), which a number has none of.
				throw BinderException(fallback_location, "else '%s' stands in a collated text payload", fallback_text);
			}
			auto &payload_type = reducer->children[0]->return_type;
			LogicalType common;
			if (!LogicalType::TryGetMaxLogicalType(context, payload_type, fallback->return_type, common)) {
				throw BinderException(fallback_location, "%s", NotANumberMessage(fallback_text));
			}
			if (payload_type != common) {
				auto payload = BoundCastExpression::AddCastToType(context, std::move(reducer->children[0]), common);
				ErrorData reducer_error;
				result = BindFrameAggregate(name, std::move(payload), frame, reducer->GetAlias(), reducer_error);
				if (!result) {
					return BindResult(std::move(reducer_error));
				}
			}
			// The type the bound reducer reads, which its overload may have widened the payload to (`sum` of a FLOAT
			// reads a DOUBLE): the fallback is one more value of it.
			auto &argument_type = GetBoundAggregate(*result)->children[0]->return_type;
			spec.fallback = BoundCastExpression::AddCastToType(context, std::move(fallback), argument_type);
		}
	} else {
		ErrorData payload_error;
		BindChild(frame.payload, depth, payload_error);
		if (payload_error.HasError()) {
			return BindResult(std::move(payload_error));
		}
		vector<unique_ptr<Expression>> children;
		children.push_back(std::move(BoundExpression::GetExpression(*frame.payload)));
		// The point reads its payload where it selects a row and its fallback where it does
		// not, so its value takes a type that holds both: `else 0.5` over a whole-numbered
		// payload is 0.5 there. The fallback is bound first for that reason.
		unique_ptr<Expression> fallback;
		auto fallback_error = BindFrameFallback(frame, fallback_text, depth, fallback);
		if (fallback_error.HasError()) {
			return BindResult(std::move(fallback_error));
		}
		if (fallback) {
			auto &payload_type = children[0]->return_type;
			LogicalType common;
			if (!LogicalType::TryGetMaxLogicalType(context, payload_type, fallback->return_type, common)) {
				throw BinderException(fallback_location, "%s", NotANumberMessage(fallback_text));
			}
			children[0] = BoundCastExpression::AddCastToType(context, std::move(children[0]), common);
			spec.fallback = BoundCastExpression::AddCastToType(context, std::move(fallback), common);
		}
		ErrorData first_error;
		result = BindFrameAggregate("first", std::move(children[0]), frame, string(), first_error);
		if (!result) {
			return BindResult(std::move(first_error));
		}
	}
	auto *aggregate = GetBoundAggregate(*result);
	if (!aggregate || aggregate->children.empty()) {
		throw InternalException("DECIDE: a frame selector did not bind to an aggregate over its payload");
	}

	// The order key binds as plain SQL, as a `when` condition does.
	auto order_error = BindDataChild(frame.order, depth);
	if (order_error.HasError()) {
		return BindResult(std::move(order_error));
	}
	spec.order = std::move(BoundExpression::GetExpression(*frame.order));
	// A frame orders peers as ORDER BY does (D74): under the key's collation, or the session's default where it has
	// none, settled here as a bound ORDER BY's and a key's are.
	unique_ptr<Expression> collated = make_uniq<BoundReferenceExpression>(spec.order->return_type, 0U);
	if (ExpressionBinder::PushCollation(context, collated, spec.order->return_type)) {
		spec.order_collation = std::move(collated);
	}
	// The within key resolves as a per key does -- columns, and relations standing for theirs -- into the key table,
	// so a row whose written `within` column is NULL is in no timeline (D5, D75). Messages name its elements and the
	// order key as written (DecideFrameSpec::timeline_names, order_name).
	if (!qualifier_context->keys) {
		throw InternalException("DECIDE: a frame selector needs the clause's keys");
	}
	vector<string> within_text;
	for (auto &element : frame.within) {
		within_text.push_back(DecideWrittenSql(*element, source_text));
	}
	spec.within_key = FindOrCreateDecideKey(
	    KeyElements(context, ResolveKey(binder, DecideKeyView(frame.within.data(), frame.within.size()),
	                                    DecisionNames(variables), source_text)),
	    StringUtil::Join(within_text, ", "), *qualifier_context->keys);

	// The aggregate prints as the frame written: DecideBinder::BindExpression gives it the frame's query location.
	auto &frames = *qualifier_context->frames;
	AddDecideTag(aggregate->alias, MakeFrameTag(frames.size()));
	frames.push_back(std::move(spec));
	return BindResult(std::move(result));
}

BindResult DecideBinder::BindFunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	auto &expr = *expr_ptr;
	auto &function = expr.Cast<FunctionExpression>();
	if (IsDecideRelationPrefix(expr)) {
		return BindQualifiedReducer(function, depth);
	}
	// `by (key)` and a scope in front of a reducer's argument. ValidateDecideScopeSpellings
	// has already refused both on anything but SUM, AVG, MIN and MAX.
	if (IsDecideBy(expr)) {
		return BindReducerBy(function, depth);
	}
	if (IsDecideScopedCall(expr)) {
		return BindScopedReducer(expr_ptr, depth);
	}
	// NORM is DECIDE syntax, not a catalog function. Bind it as a deliberately
	// inert SUM aggregate marker; DecideOptimizer owns the mathematical rewrite
	// and replaces this marker before it can reach physical planning. Keeping an
	// aggregate-shaped marker is important: a `by (G)` after it uses the same
	// bound-tree contract as on any other reducer.
	if (!function.is_operator && StringUtil::CIEquals(function.function_name, "norm")) {
		auto text = qualifier_context->source_text;
		if (function.children.size() < 2 || function.children.size() > 3) {
			return BindResult(BinderException(
			    function, "%s", ArgumentCountMessage(DecideWrittenSql(function, text), function.children.size())));
		}
		auto &order_argument = *function.children[1];
		if (order_argument.GetExpressionClass() != ExpressionClass::CONSTANT) {
			return BindResult(BinderException(order_argument, "the order of '%s' is not a constant",
			                                  DecideWrittenSql(function, text)));
		}
		auto &order_value = order_argument.Cast<ConstantExpression>().value;
		string payload;
		if (order_value.type().id() == LogicalTypeId::VARCHAR) {
			auto order = StringUtil::Lower(StringValue::Get(order_value));
			if (order != "inf" && order != "infinity" && order != "max") {
				return BindResult(NotImplementedException(order_argument, "norm order '%s' is not supported",
				                                          StringValue::Get(order_value)));
			}
			if (function.children.size() != 2) {
				return BindResult(BinderException(*function.children[2], "only norm order 0 takes a bound"));
			}
			payload = "inf";
		} else if (order_value.type().IsNumeric()) {
			double order = order_value.GetValue<double>();
			if (order == 1.0) {
				if (function.children.size() != 2)
					return BindResult(BinderException(*function.children[2], "only norm order 0 takes a bound"));
				payload = "1";
			} else if (order == 2.0) {
				if (function.children.size() != 2)
					return BindResult(BinderException(*function.children[2], "only norm order 0 takes a bound"));
				payload = "2";
			} else if (order == 0.0) {
				if (function.children.size() == 2) {
					payload = "0_auto";
				} else {
					auto &bound_argument = *function.children[2];
					if (bound_argument.GetExpressionClass() != ExpressionClass::CONSTANT ||
					    !(bound_argument.Cast<ConstantExpression>().value.GetValue<double>() > 0.0)) {
						return BindResult(BinderException(bound_argument,
						                                  "the bound of '%s' is not a positive constant",
						                                  DecideWrittenSql(function, text)));
					}
					auto m = bound_argument.Cast<ConstantExpression>().value.GetValue<double>();
					payload = "0_" + StringUtil::Format("%.17g", m);
				}
			} else {
				return BindResult(NotImplementedException(order_argument, "norm order '%s' is not supported",
				                                          DecideWrittenSql(order_argument, text)));
			}
		} else {
			return BindResult(NotImplementedException(order_argument, "norm order '%s' is not supported",
			                                          DecideWrittenSql(order_argument, text)));
		}
		// An L0 norm of a decision stays a marker for the optimizer, which gives it indicators; no row reads it as a
		// value.
		const bool stays_a_marker =
		    StringUtil::StartsWith(payload, "0") && ExpressionContainsDecideVariable(*function.children[0], variables);
		vector<unique_ptr<ParsedExpression>> marker_children;
		marker_children.push_back(std::move(function.children[0]));
		// The system's SUM by name: a norm is DECIDE syntax, so a user's `sum` macro does not capture it.
		auto marker = make_uniq<FunctionExpression>(SYSTEM_CATALOG, DEFAULT_SCHEMA, "sum", std::move(marker_children));
		marker->alias = function.alias;
		marker->SetQueryLocation(function.GetQueryLocation());
		AddDecideTag(marker->alias, string(NORM_MARKER_TAG_PREFIX) + payload + "__");
		expr_ptr = std::move(marker);
		auto result = BindFunction(expr_ptr, depth);
		if (!result.HasError() && !stays_a_marker) {
			// The marker parses as SUM(e), but the norm's value is its definition's, which what reads it binds to. Its
			// type needs only the body's: a body holding a subquery cannot be copied until planning has run.
			auto &aggregate = *GetBoundAggregate(*result.expression);
			auto body = make_uniq<BoundReferenceExpression>(aggregate.children[0]->return_type, 0U);
			auto type = DecideNormDefinition(context, std::move(body), payload)->return_type;
			aggregate.function.return_type = type;
			aggregate.return_type = std::move(type);
		}
		return result;
	}
	// Check if this is an aggregate function
	QueryErrorContext error_context(expr_ptr->GetQueryLocation());
	auto &catalog_entry = *GetCatalogEntry(CatalogType::SCALAR_FUNCTION_ENTRY, function.catalog, function.schema,
	                                       function.function_name, OnEntryNotFound::THROW_EXCEPTION, error_context);

	if (catalog_entry.type == CatalogType::AGGREGATE_FUNCTION_ENTRY) {
		// It's an aggregate function, bind it using our custom aggregate logic
		return BindAggregate(function, catalog_entry.Cast<AggregateFunctionCatalogEntry>(), depth);
	}
	// It's a scalar function, bind it normally. A NULL argument makes it NULL, and DuckDB
	// folds it to that constant: the decisions it read are gone, so it is refused here,
	// where the SQL is still at hand.
	bool reads_decision = ExpressionContainsDecideVariable(function, variables);
	auto text = reads_decision ? DecideWrittenSql(function, qualifier_context->source_text) : string();
	auto result = ExpressionBinder::BindExpression(expr_ptr, depth);
	if (reads_decision && !result.HasError() &&
	    result.expression->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
	    result.expression->Cast<BoundConstantExpression>().value.IsNull()) {
		throw BinderException(function, "'%s' is NULL whatever the decisions are", text);
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
	auto &expr = *expr_ptr;
	if (expr.GetExpressionClass() == ExpressionClass::DECIDE_FRAME) {
		return BindDecideFrame(expr.Cast<DecideFrameExpression>(), depth);
	}
	if (expr.GetExpressionClass() == ExpressionClass::WINDOW) {
		return BindUnsupportedExpression(expr, depth, "a window function is not allowed in a DECIDE clause");
	}
	// The constraints and objective binders call this for a frame, at any depth, and for a subquery they met at depth
	// 0, and bind every other class themselves.
	D_ASSERT(depth == 0 && expr.GetExpressionClass() == ExpressionClass::SUBQUERY);
	if (expr.Cast<SubqueryExpression>().subquery_type != SubqueryType::SCALAR) {
		return BindResult(BinderException(expr, "%s", SubqueryNotScalarMessage()));
	}
	if (ExpressionContainsDecideVariable(expr, variables)) {
		return BindResult(BinderException(expr, "%s", SubqueryReadsDecisionMessage()));
	}
	// Standard binding handles both correlated and uncorrelated scalar subqueries.
	// Uncorrelated: PlanSubqueries evaluates them as cross-joined scalars.
	// Correlated: PlanSubqueries decorrelates them into joins, producing
	//             per-row values that the DECIDE operator evaluates normally.
	return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
}

} // namespace duckdb
