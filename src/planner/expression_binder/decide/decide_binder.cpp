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
	static const struct {
		const char *tag;
		const char *spelling;
	} scopes[] = {
	    {SCOPE_WHEN_TAG, "when"}, {SCOPE_PER_TAG, "per"}, {SCOPE_PER_ROW_TAG, "per row"}, {REDUCER_BY_TAG, "by"}};
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		for (auto &scope : scopes) {
			if (func.is_operator && func.function_name == scope.tag) {
				bool global = func.function_name == SCOPE_PER_TAG && func.children.size() == 1;
				throw NotImplementedException(expr, "'%s' inside a subquery is not supported",
				                              global ? "per ()" : scope.spelling);
			}
		}
		if (IsDecideMarker(expr, QUALIFIED_REDUCER_TAG)) {
			throw NotImplementedException(expr, "'%s' inside a subquery is not supported",
			                              DecideWrittenSql(expr, text));
		}
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

//! A power that is no first power: `power(e, k)`, `pow`, `e ** k`, `e ^ k` with `k` other
//! than the constant 1.
static bool IsNonlinearPower(const FunctionExpression &func) {
	auto name = StringUtil::Lower(func.function_name);
	bool power = (func.is_operator && (name == "**" || name == "^")) ||
	             (!func.is_operator && (name == "power" || name == "pow"));
	if (!power || func.children.size() != 2) {
		return false;
	}
	auto &exponent = *func.children[1];
	if (exponent.GetExpressionClass() == ExpressionClass::CONSTANT) {
		auto &value = exponent.Cast<ConstantExpression>().value;
		return value.IsNull() || !value.type().IsNumeric() || value.GetValue<double>() != 1.0;
	}
	return true;
}

//! Whether `expr` squares or multiplies decisions: a product with a decision in two of
//! its factors, or a power of a decision other than the first.
static bool MultipliesDecisions(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<FunctionExpression>();
		if (IsNonlinearPower(func) && ExpressionContainsDecideVariable(*func.children[0], variables)) {
			return true;
		}
		if (func.is_operator && func.function_name == "*") {
			idx_t factors = 0;
			for (auto &child : func.children) {
				factors += ExpressionContainsDecideVariable(*child, variables) ? 1 : 0;
			}
			if (factors > 1) {
				return true;
			}
		}
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { found = found || MultipliesDecisions(child, variables); });
	return found;
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

//! The first node of `expr` that holds a decision and reads it other than linearly: a call
//! other than `+`, `-`, `*` and `/` over one (`abs(x - 3)`), or a decision divisor. A data
//! subtree, whatever it computes, is a coefficient. Null when `expr` is linear.
static optional_ptr<const ParsedExpression> FindNonlinearDecisionUse(const ParsedExpression &expr,
                                                                     const case_insensitive_map_t<idx_t> &variables) {
	if (!ExpressionContainsDecideVariable(expr, variables) ||
	    expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		return nullptr;
	}
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return &expr;
	}
	auto &func = expr.Cast<FunctionExpression>();
	auto name = func.function_name;
	if (!func.is_operator || (name != "+" && name != "-" && name != "*" && name != "/")) {
		return &expr;
	}
	if (name == "/" && func.children.size() == 2 && ExpressionContainsDecideVariable(*func.children[1], variables)) {
		return &expr;
	}
	for (auto &child : func.children) {
		auto found = FindNonlinearDecisionUse(*child, variables);
		if (found) {
			return found;
		}
	}
	return nullptr;
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
			// A frame's term is linear: its payload is read at another row, and a square
			// there is a product across rows once the frame is written out (D80).
			if (MultipliesDecisions(*value, variables)) {
				throw BinderException(*value,
				                      "'%s' squares or multiplies decisions inside a frame, which is not allowed",
				                      DecideWrittenSql(*value, text));
			}
			// So is anything else over a decision a rewrite would lower into rows of its own:
			// those rows stand at the anchor, while the frame reads its payload elsewhere.
			auto nonlinear = FindNonlinearDecisionUse(*value, variables);
			if (nonlinear) {
				throw NotImplementedException(
				    *nonlinear, "'%s' inside a frame is not linear in the decisions, which is not supported",
				    DecideWrittenSql(*nonlinear, text));
			}
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
	const ParsedExpression *body = &expr;
	if (IsDecideMarker(*body, SCOPE_PER_ROW_TAG)) {
		body = body->Cast<const FunctionExpression>().children[0].get();
	} else if (IsDecideMarker(*body, SCOPE_PER_TAG)) {
		auto &per = body->Cast<const FunctionExpression>();
		for (idx_t i = 1; i < per.children.size(); i++) {
			ValidateScopeData(*per.children[i], variables, text);
		}
		body = per.children[0].get();
	}
	if (IsDecideMarker(*body, SCOPE_WHEN_TAG)) {
		auto &when = body->Cast<const FunctionExpression>();
		ValidateScopeData(*when.children[1], variables, text);
		body = when.children[0].get();
	}
	return *body;
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
	auto &func = expr.Cast<const FunctionExpression>();
	if (func.is_operator && func.function_name == REDUCER_BY_TAG) {
		if (!TakesReducerBy(UnwrapReducerMarkers(*func.children[0]))) {
			throw BinderException(expr, "by (...) does not follow a reducer");
		}
		for (idx_t i = 1; i < func.children.size(); i++) {
			ValidateScopeData(*func.children[i], placement.variables, placement.text);
		}
		ValidateScopePlacement(*func.children[0], placement);
		return;
	}
	bool scoped_argument = !func.is_operator && !func.children.empty() && IsDecideScopeMarker(*func.children[0]);
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

static void ValidateScopeClause(const ParsedExpression &clause, const case_insensitive_map_t<idx_t> &variables,
                                const DecideSourceText *text) {
	auto &body = PeelScopeSpelling(clause, variables, text);
	// Every clause follows the scope rules (D30).
	ValidateScopePlacement(body, ScopePlacement {variables, text});
}

void ValidateDecideScopeSpellings(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                  bool objective, const DecideSourceText *text) {
	if (!objective && expr.GetExpressionClass() == ExpressionClass::CONJUNCTION &&
	    expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		// `AND` separates constraints; each is checked on its own.
		for (auto &child : expr.Cast<const ConjunctionExpression>().children) {
			ValidateDecideScopeSpellings(*child, variables, objective, text);
		}
		return;
	}
	ValidateScopeClause(expr, variables, text);
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

string ReadsNoDecisionMessage(const string &expression) {
	return StringUtil::Format("'%s' reads no decision", expression);
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

string FractionalComparisonMessage(const string &comparison, const string &operand) {
	return StringUtil::Format("'%s' compares '%s', which can take fractional values", comparison, operand);
}

BindResult DecideRefusal::ToResult(const ParsedExpression &fallback) const {
	auto &node = at ? *at : fallback;
	if (not_implemented) {
		return BindResult(NotImplementedException(node, "%s", message));
	}
	return BindResult(BinderException(node, "%s", message));
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

//! The columns of a key listed by `elements[first..]` (none for `()`), each element resolved
//! by ResolveKeyElement.
static KeyColumns ResolveKey(Binder &binder, const vector<unique_ptr<ParsedExpression>> &elements, idx_t first,
                             const case_insensitive_set_t &decision_names, const DecideSourceText *text) {
	KeyColumns columns;
	for (idx_t i = first; i < elements.size(); i++) {
		ResolveKeyElement(binder, elements[i]->Cast<ColumnRefExpression>(), decision_names, columns, text);
	}
	return columns;
}

//! The columns of the key `per` lists: its children[1..].
static KeyColumns ResolveKey(Binder &binder, const FunctionExpression &per,
                             const case_insensitive_set_t &decision_names, const DecideSourceText *text) {
	return ResolveKey(binder, per.children, 1, decision_names, text);
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

idx_t FindOrCreateKeyScope(ClientContext &context, Binder &binder, const FunctionExpression &per,
                           const case_insensitive_set_t &decision_names, const string &key_text,
                           const DecideSourceText *text, vector<DecideKey> &keys,
                           vector<KeyedScopeInfo> &keyed_scopes) {
	auto key = BindKey(context, binder, per, decision_names, key_text, text, keys);
	return FindOrCreateScope(key, keyed_scopes);
}

idx_t BindKey(ClientContext &context, Binder &binder, const FunctionExpression &per,
              const case_insensitive_set_t &decision_names, const string &key_text, const DecideSourceText *text,
              vector<DecideKey> &keys) {
	return FindOrCreateDecideKey(KeyElements(context, ResolveKey(binder, per, decision_names, text)), key_text, keys);
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
		// A condition, a key or a `by` key is not part of the value.
		if (func.is_operator && func.function_name == REDUCER_BY_TAG) {
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

// True when the function name resolves to an AGGREGATE in the catalog. These
// are rejected (with an aggregate-specific error) in BindAggregate, so we must
// not also false-flag them here as "non-linear scalars".
static bool IsAggregateFunctionName(ClientContext &context, const FunctionExpression &func) {
	auto entry = Catalog::GetEntry(context, CatalogType::SCALAR_FUNCTION_ENTRY, func.catalog, func.schema,
	                               func.function_name, OnEntryNotFound::RETURN_NULL);
	return entry && entry->type == CatalogType::AGGREGATE_FUNCTION_ENTRY;
}

// The first aggregate over table columns only (no DECIDE variable anywhere in its
// argument) in `expr` -- e.g. `AVG(price)`, `MIN(cost)` -- or null. Such a
// value needs the whole row set, so it is a scalar the query must pre-compute,
// not a per-row coefficient. Recurses so nested forms (`AVG(p) * 2`) are caught,
// but not into a reducer over decisions: its operand is a scoped expression of
// its own (deck p. 29), checked where the walk reaches it, so in
// `2 * sum(x - avg(v) by (h)) by (g)` the `avg` scales no decision.
static optional_ptr<const ParsedExpression> FindDataOnlyAggregate(ClientContext &context, const ParsedExpression &expr,
                                                                  const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (IsAggregateFunctionName(context, func)) {
			return ExpressionContainsDecideVariable(expr, variables) ? nullptr : &expr;
		}
	}
	optional_ptr<const ParsedExpression> found;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (!found) {
			found = FindDataOnlyAggregate(context, child, variables);
		}
	});
	return found;
}

//! `in_reducer`: `expr` stands in the argument of a reducer over decisions. There a reducer over data alone is a value
//! of its path's group, a coefficient like any data (language/guide.md §5.5; deck p. 51), so it may scale or divide a
//! decision.
static void ValidateDecideNoDataAggregateFactor(ClientContext &context, const ParsedExpression &expr,
                                                const case_insensitive_map_t<idx_t> &variables,
                                                const DecideSourceText *text, bool in_reducer) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (!in_reducer && func.function_name == "*" && ExpressionContainsDecideVariable(expr, variables)) {
			// A data-only aggregate multiplied by a decision variable
			// (`SUM(avg(p) * x)`) is not a per-row coefficient: the aggregate
			// spans all rows. Symbolic normalization would silently distribute the
			// variable into it (`avg(p)*x` -> `avg(p*x)`, a different constraint) and
			// return a wrong optimum, so reject it here — before normalization —
			// consistent with the direct `SUM(sum(p) * x)` rejection.
			auto data_aggregate = FindDataOnlyAggregate(context, expr, variables);
			if (data_aggregate) {
				throw BinderException(*data_aggregate, "'%s' reduces data, which cannot multiply a decision",
				                      DecideWrittenSql(*data_aggregate, text));
			}
		}
		// A data-only aggregate stays on the bound side (S10). Like the multiplier
		// above, it may not share a scaled term with a decision: as the divisor, or
		// inside the dividend (`(sum(x) + sum(cap)) / 2`).
		if (!in_reducer && func.function_name == "/" && ExpressionContainsDecideVariable(expr, variables)) {
			auto data_aggregate = FindDataOnlyAggregate(context, expr, variables);
			if (data_aggregate) {
				auto divisor = FindDataOnlyAggregate(context, *func.children[1], variables);
				auto &culprit = divisor ? *divisor : *data_aggregate;
				throw BinderException(culprit, "'%s' reduces data, which cannot %s a decision",
				                      DecideWrittenSql(culprit, text), divisor ? "divide" : "be divided together with");
			}
		}
	}
	bool below = in_reducer;
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		below = below || (IsAggregateFunctionName(context, func) && ExpressionContainsDecideVariable(expr, variables));
	}
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		ValidateDecideNoDataAggregateFactor(context, child, variables, text, below);
	});
}

void ValidateDecideNoDataAggregateFactor(ClientContext &context, const ParsedExpression &expr,
                                         const case_insensitive_map_t<idx_t> &variables, const DecideSourceText *text) {
	ValidateDecideNoDataAggregateFactor(context, expr, variables, text, false);
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
static string FindRealDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                     const vector<LogicalType> &variable_types) {
	if (IsIntegerValuedReducer(expr)) {
		return "";
	}
	if (IsVariableExpression(expr, variables)) {
		// A decision is DOUBLE, INTEGER (BOOL) or BIGINT (INT), as the declarations binder types it.
		const auto &colref = expr.Cast<const ColumnRefExpression>();
		if (variable_types[variables.find(colref.GetColumnName())->second] == LogicalType::DOUBLE) {
			return colref.GetColumnName();
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
                                                 const vector<LogicalType> &variable_types,
                                                 const DecideSourceText *text) {
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
			ValidateDecideNoIntegerStepComparisonOnReal(*child, variables, variable_types, text);
		}
		return;
	}
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<const FunctionExpression>();
		if (IsDecideScopeMarker(expr)) {
			// children[0] is the constraint; the rest are the condition / key.
			ValidateDecideNoIntegerStepComparisonOnReal(*func.children[0], variables, variable_types, text);
		}
		return;
	}
	case ExpressionClass::COMPARISON: {
		auto type = expr.GetExpressionType();
		bool is_strict = type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_GREATERTHAN;
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
			// `<>` over a continuous quantity has no exact encoding at all (an open set is no MILP region); `<` and
			// `>` have the step. One text covers the three.
			const char *comparison = is_not_equal ? "<>" : type == ExpressionType::COMPARE_LESSTHAN ? "<" : ">";
			throw NotImplementedException(comp, "%s", FractionalComparisonMessage(comparison, real_var));
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
	// A range frame's marker is a value of the row (language/guide.md §5.6), which no rewrite multiplies through.
	auto &agg = expr.Cast<const BoundAggregateExpression>();
	idx_t frame;
	return StringUtil::CIEquals(agg.function.name, "avg") && !TryParseFrameTag(agg.GetAlias(), frame);
}

static bool DecideSideIsWholeNumbered(const Expression &expr, bool allow_avg_hoist, idx_t decide_index,
                                      bool addends_count = false);

//! The value of a constant exponent, through the casts the binder put on it.
static bool TryGetConstantExponent(const Expression &expr, double &out_value) {
	const Expression *cur = &expr;
	while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		cur = cur->Cast<const BoundCastExpression>().child.get();
	}
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	try {
		out_value =
		    cur->Cast<const BoundConstantExpression>().value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	} catch (...) {
		return false;
	}
	return true;
}

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
		return DecideSideIsWholeNumbered(*agg.children[0], false, decide_index, addends_count);
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
		// A frame is a value of the row, every addend and its `else` part of it: whole when the type its payload and
		// `else` share is (BindDecideFrame casts both to it), and a range's SUM, MIN or MAX of them; an AVG range
		// divides.
		idx_t frame;
		if (TryParseFrameTag(agg.GetAlias(), frame)) {
			return !StringUtil::CIEquals(name, "avg") && agg.children.size() == 1 &&
			       DecideTypeIsWholeNumbered(agg.children[0]->return_type);
		}
		bool extremum = StringUtil::CIEquals(name, "min") || StringUtil::CIEquals(name, "max");
		bool norm = HasDecideTag(agg.alias, NORM_MARKER_TAG_PREFIX);
		if (extremum && agg.children.size() == 1) {
			// On the side's additive spine its constant addends move to the bound (stage 04); under a power or a
			// product of decisions they stay inside, and the whole body counts.
			return addends_count ? DecideSideIsWholeNumbered(*agg.children[0], false, decide_index, true)
			                     : DecideExtremumBodyIsWholeNumbered(*agg.children[0], decide_index);
		}
		if (StringUtil::CIEquals(name, "sum")) {
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
		// degree at all and DecideShapeCheck refuses it, so it need not be answered here.
		if (name == "**") {
			double exp_val;
			if (TryGetConstantExponent(*func.children[1], exp_val) && exp_val >= 0.0 &&
			    exp_val == std::floor(exp_val)) {
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

//! First operand that is not whole-numbered, for the message. Descends the same way the
//! test above does, so what it reports is what was refused.
static bool FindFractionalOperand(const Expression &expr, bool allow_avg_hoist, idx_t decide_index,
                                  const DecideSourceNames &names, string &out) {
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
		out = expr.Cast<const BoundColumnRefExpression>().GetName();
		return true;
	}
	// No fractional leaf underneath: the node itself is what produces fractions, named as
	// written (its reducer's alias carries the source tags).
	out = RenderDecideSource(expr, names);
	return true;
}

void ValidateDecideIntegralComparisonOperands(const Expression &expr, idx_t decide_index,
                                              const DecideSourceNames &names) {
	// Constraint position only, and by the same reasoning as the parsed-tree validator:
	// a comparison nested inside an operand is a boolean value, not a model row.
	// ForEachConstraintLeaf owns that descent; this is only the check.
	ForEachConstraintLeaf(expr, [&](const Expression &node) {
		if (node.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return;
		}
		auto type = node.GetExpressionType();
		bool is_strict = type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_GREATERTHAN;
		bool is_not_equal = type == ExpressionType::COMPARE_NOTEQUAL;
		if (!is_strict && !is_not_equal) {
			return;
		}
		auto &comp = node.Cast<const BoundComparisonExpression>();
		// Only a side that carries decisions is a compared quantity; the other is the
		// bound, and a fractional bound is a tautology rather than an error.
		bool left_is_compared = SideReferencesDecision(*comp.left, decide_index);
		bool right_is_compared = SideReferencesDecision(*comp.right, decide_index);
		string bad;
		if (!(left_is_compared && FindFractionalOperand(*comp.left, is_not_equal, decide_index, names, bad)) &&
		    !(right_is_compared && FindFractionalOperand(*comp.right, is_not_equal, decide_index, names, bad))) {
			return;
		}
		const char *op = is_not_equal ? "<>" : type == ExpressionType::COMPARE_LESSTHAN ? "<" : ">";
		// Pass the comparison so the message carries a caret (DecideBinder::BindExpression stamps locations).
		throw NotImplementedException(comp, "%s", FractionalComparisonMessage(op, bad));
	});
}

string DecideWrittenFunctionName(const FunctionExpression &func) {
	return StringUtil::CIEquals(func.function_name, "count_star") ? "count(*)"
	                                                              : DecideWrittenName(func.alias, func.function_name);
}

//! A reducer's argument, read for what only the binder can see: a subquery that is not scalar or that reads a
//! decision, a truth value over a decision, and an ABS of the wrong arity. Sets `has_decide_variable` when the
//! argument reads a decision. Its algebra is DecideShapeCheck's, after canonicalization.
static bool ValidateSumArgumentInternal(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                        const DecideSourceText *text, bool &has_decide_variable,
                                        DecideRefusal &refusal) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF:
		if (IsVariableExpression(expr, variables)) {
			has_decide_variable = true;
		}
		return true;
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		auto func_name_lower = StringUtil::Lower(func.function_name);
		if (func.is_operator) {
			break;
		}
		if (func_name_lower == "@" && func.children.size() != 1) {
			refusal.Set(expr, ArgumentCountMessage(DecideWrittenSql(expr, text), func.children.size()));
			return false;
		}
		if (IsDecideAggregateName(func.function_name)) {
			// A nested reducer (language/guide.md §5.5): its body is held to the rule of any reducer's.
			if (func.children.size() == 1 && ExpressionContainsDecideVariable(*func.children[0], variables)) {
				auto &body = const_cast<ParsedExpression &>(ReducerArgumentBody(*func.children[0]));
				return ValidateSumArgumentInternal(body, variables, text, has_decide_variable, refusal);
			}
			// One over data alone is a value of its path's group (RewriteRhsAggregates).
			return true;
		}
		break;
	}
	case ExpressionClass::SUBQUERY: {
		auto &subquery = expr.Cast<SubqueryExpression>();
		if (subquery.subquery_type != SubqueryType::SCALAR) {
			refusal.Set(expr, SubqueryNotScalarMessage());
			return false;
		}
		if (ExpressionContainsDecideVariable(expr, variables)) {
			refusal.Set(expr, SubqueryReadsDecisionMessage());
			return false;
		}
		return true;
	}
	case ExpressionClass::COMPARISON:
	case ExpressionClass::BETWEEN:
	case ExpressionClass::CONJUNCTION:
		// A truth value over a decision is no term of a sum; over data it is a 0/1 coefficient.
		if (ExpressionContainsDecideVariable(expr, variables)) {
			refusal.Set(expr, ComparisonUsedAsNumberMessage(DecideWrittenSql(expr, text)));
			return false;
		}
		return true;
	default:
		break;
	}
	bool valid = true;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](ParsedExpression &child) {
		valid = valid && ValidateSumArgumentInternal(child, variables, text, has_decide_variable, refusal);
	});
	return valid;
}

bool ValidateSumArgument(ParsedExpression &reducer, ParsedExpression &argument,
                         const case_insensitive_map_t<idx_t> &variables, const DecideSourceText *text,
                         DecideRefusal &refusal) {
	bool has_decide_variable = false;
	if (!ValidateSumArgumentInternal(argument, variables, text, has_decide_variable, refusal)) {
		return false;
	}
	if (!has_decide_variable) {
		refusal.Set(reducer, ReadsNoDecisionMessage(DecideWrittenSql(reducer, text)));
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

bool DecideBinder::ClassifyReducerCall(FunctionExpression &func, DecideExpression &result, DecideRefusal &refusal) {
	auto fname = StringUtil::Lower(func.function_name);
	auto text = qualifier_context->source_text;
	if (fname == "norm") {
		if (func.children.empty()) {
			refusal.Set(func, ArgumentCountMessage(DecideWrittenSql(func, text), 0));
			result = DecideExpression::INVALID;
		} else if (!ValidateSumArgument(func, *func.children.front(), variables, text, refusal)) {
			result = DecideExpression::INVALID;
		} else {
			result = DecideExpression::SUM;
		}
		return true;
	}
	if (IsDecideAggregateName(fname)) {
		// A reducer takes one argument: MAX(x, n) and MIN(x, n) are DuckDB's top-n aggregates, which return a list.
		if (func.children.size() != 1) {
			refusal.Set(func, ArgumentCountMessage(DecideWrittenSql(func, text), func.children.size()));
			result = DecideExpression::INVALID;
			return true;
		}
		// The value reduced is the argument's body; a `when` or `per` in front of it only
		// chooses the rows.
		auto &body = const_cast<ParsedExpression &>(ReducerArgumentBody(*func.children.front()));
		if (ExpressionContainsDecideVariable(body, variables) && IsRowInvariantExpression(body)) {
			refusal.Set(func, StringUtil::Format("'%s' is a query-wide decision, so '%s' has no rows to reduce",
			                                     DecideWrittenSql(body, text), DecideWrittenSql(func, text)));
			result = DecideExpression::INVALID;
		} else if (!ValidateSumArgument(func, body, variables, text, refusal)) {
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
		return BindResult(NotImplementedException(aggr, "'%s' is not supported in a DECIDE clause",
		                                          DecideWrittenSql(aggr, qualifier_context->source_text)));
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
	// The grammar makes the qualifier a list of single names (makeQualifiedReducer), one or more.
	vector<string> relations;
	for (idx_t i = 1; i < qualified_expr.children.size(); i++) {
		auto &qualifier = qualified_expr.children[i]->Cast<ColumnRefExpression>();
		relations.push_back(qualifier.GetColumnName());
	}
	auto relation_list = StringUtil::Join(relations, ", ");

	KeyColumns key;
	for (idx_t i = 0; i < relations.size(); i++) {
		ErrorData binding_error;
		auto binding = binder.bind_context.GetBinding(relations[i], binding_error);
		if (!binding || binding->index == ctx.decide_index) {
			auto &qualifier = *qualified_expr.children[i + 1];
			return BindResult(BinderException(qualifier, "'%s' is not a relation of the FROM clause",
			                                  DecideWrittenSql(qualifier, ctx.source_text)));
		}
		AddRelationColumns(*binding, key);
	}
	// `SUM(D, T: e)` is `sum(per D, T: e)` (language/guide.md §5.1): the same exact scope, so the same
	// classes and the same variant signature (D70).
	auto scope_idx = FindOrCreateScope(FindOrCreateDecideKey(KeyElements(context, key), relation_list, *ctx.keys),
	                                   *ctx.keyed_scopes);

	auto aggregate_result = BindExpression(qualified_expr.children[0], depth);
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
		auto condition_error = BindDataChild(condition, depth);
		if (condition_error.HasError()) {
			return BindResult(std::move(condition_error));
		}
		aggregate->filter = BoundCastExpression::AddCastToType(
		    context, std::move(BoundExpression::GetExpression(*condition)), LogicalType::BOOLEAN);
	}
	if (per) {
		auto &marker = per->Cast<FunctionExpression>();
		auto scope_idx = FindOrCreateKeyScope(context, binder, marker, DecisionNames(variables),
		                                      DecideKeyToString(marker), qualifier_context->source_text,
		                                      *qualifier_context->keys, *qualifier_context->keyed_scopes);
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
	// Each element is a data expression, or a relation standing for its stored columns.
	vector<DecideKeyElement> key;
	for (idx_t i = 1; i < by.children.size(); i++) {
		auto &element = by.children[i];
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
	    KeyElements(context, ResolveKey(binder, frame.within, 0, DecisionNames(variables), source_text)),
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
