#include "duckdb/parser/decide/decide_parsed_markers.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

namespace duckdb {

//! The transformer writes each construct as an operator function named by a tag (transform_operator.cpp): the
//! body or call is children[0]; a `when` holds its condition in children[1]; a `per`, a `by` and a relation prefix
//! hold their key in children[1..]. With both a `when` and a `per`, the `per` is the outer one.
static bool IsDecideMarker(const ParsedExpression &expr, const char *tag) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<FunctionExpression>();
	return func.is_operator && func.function_name == tag && !func.children.empty();
}

//! The marker `expr`, whose children the views point into: they are reached through unique_ptrs, which a const
//! parent does not make const.
static FunctionExpression &Marker(const ParsedExpression &expr) {
	return const_cast<ParsedExpression &>(expr).Cast<FunctionExpression>(); // NOLINT
}

//! The children of `marker` after its body.
static DecideKeyView MarkerKey(FunctionExpression &marker) {
	return DecideKeyView(marker.children.data() + 1, marker.children.size() - 1);
}

string DecideKeyView::ToString() const {
	string result;
	for (idx_t i = 0; i < count; i++) {
		result += (i > 0 ? ", " : "") + first[i]->ToString();
	}
	return result;
}

string DecideScopeView::ToString() const {
	string result = when ? "WHEN " + (*when)->ToString() : string();
	if (per == DecidePer::NONE) {
		return result;
	}
	result += result.empty() ? "" : " ";
	switch (per) {
	case DecidePer::ROW:
		return result + "PER ROW";
	case DecidePer::ALL:
		return result + "PER ()";
	default:
		return result + "PER " + key.ToString();
	}
}

bool IsDecideScope(const ParsedExpression &expr) {
	return IsDecideMarker(expr, SCOPE_WHEN_TAG) || IsDecideMarker(expr, SCOPE_PER_TAG) ||
	       IsDecideMarker(expr, SCOPE_PER_ROW_TAG);
}

DecideScopeView ReadDecideScope(const ParsedExpression &expr) {
	DecideScopeView scope;
	auto *marker = &Marker(expr);
	if (!IsDecideMarker(expr, SCOPE_WHEN_TAG)) {
		bool per_row = IsDecideMarker(expr, SCOPE_PER_ROW_TAG);
		scope.per = per_row ? DecidePer::ROW : marker->children.size() == 1 ? DecidePer::ALL : DecidePer::KEY;
		scope.key = per_row ? DecideKeyView() : MarkerKey(*marker);
		scope.per_location = marker->GetQueryLocation();
		scope.body = &marker->children[0];
		if (!IsDecideMarker(**scope.body, SCOPE_WHEN_TAG)) {
			return scope;
		}
		marker = &Marker(**scope.body);
	}
	if (marker->children.size() != 2) {
		throw InternalException("DECIDE WHEN scope marker has %s children, expected 2", marker->children.size());
	}
	scope.when = &marker->children[1];
	scope.when_location = marker->GetQueryLocation();
	scope.body = &marker->children[0];
	return scope;
}

bool IsDecideScopedCall(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &call = expr.Cast<FunctionExpression>();
	return !call.is_operator && !call.children.empty() && IsDecideScope(*call.children[0]);
}

bool IsDecideBy(const ParsedExpression &expr) {
	return IsDecideMarker(expr, REDUCER_BY_TAG);
}

DecideByView ReadDecideBy(const ParsedExpression &expr) {
	auto &marker = Marker(expr);
	DecideByView by;
	by.call = &marker.children[0];
	by.key = MarkerKey(marker);
	return by;
}

bool IsDecideRelationPrefix(const ParsedExpression &expr) {
	return IsDecideMarker(expr, QUALIFIED_REDUCER_TAG);
}

DecideRelationPrefixView ReadDecideRelationPrefix(const ParsedExpression &expr) {
	auto &marker = Marker(expr);
	DecideRelationPrefixView prefix;
	prefix.call = &marker.children[0];
	prefix.relations = MarkerKey(marker);
	return prefix;
}

DecideDeclarationView ReadDecideDeclaration(const ParsedExpression &declaration) {
	// The grammar builds a declaration as `name = 'type_marker'` (typed_decide_variable), the name a column
	// reference, under a `per` or `per row` scope when the declaration has one.
	auto &comparison = const_cast<ParsedExpression &>(declaration).Cast<ComparisonExpression>(); // NOLINT
	DecideDeclarationView result;
	if (IsDecideScope(*comparison.left)) {
		result.scope = ReadDecideScope(*comparison.left);
	} else {
		result.scope.body = &comparison.left;
	}
	result.name = (*result.scope.body)->Cast<ColumnRefExpression>().GetColumnName();
	auto marker = comparison.right->Cast<ConstantExpression>().value.ToString();
	if (marker == "real_variable") {
		result.type = DecideVariableType::REAL;
	} else if (marker == "integer_variable") {
		result.type = DecideVariableType::INT;
	} else if (marker == "bool_variable") {
		result.type = DecideVariableType::BOOL;
	} else {
		throw InternalException("Unknown DECIDE variable type marker '%s'", marker);
	}
	return result;
}

vector<reference<unique_ptr<ParsedExpression>>> DecideConstraintList(unique_ptr<ParsedExpression> &constraints) {
	// The grammar joins the constraints with AND (decide_constraint_list), and the transformer flattens that into
	// one conjunction.
	vector<reference<unique_ptr<ParsedExpression>>> result;
	if (constraints->GetExpressionClass() == ExpressionClass::CONJUNCTION &&
	    constraints->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &child : constraints->Cast<ConjunctionExpression>().children) {
			result.push_back(child);
		}
	} else {
		result.push_back(constraints);
	}
	return result;
}

bool IsDecideAggregateName(const string &name) {
	auto lname = StringUtil::Lower(name);
	return lname == "sum" || lname == "avg" || lname == "min" || lname == "max";
}

bool IsScopedReducerCall(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<FunctionExpression>();
	return !func.is_operator && IsDecideAggregateName(func.function_name);
}

bool TakesReducerBy(const ParsedExpression &expr) {
	if (IsScopedReducerCall(expr)) {
		return true;
	}
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<FunctionExpression>();
	return !func.is_operator &&
	       (StringUtil::CIEquals(func.function_name, "norm") || StringUtil::CIEquals(func.function_name, "count") ||
	        StringUtil::CIEquals(func.function_name, "count_star"));
}

const ParsedExpression &UnwrapReducerMarkers(const ParsedExpression &expr) {
	const ParsedExpression *result = &expr;
	if (IsDecideBy(*result)) {
		result = ReadDecideBy(*result).call->get();
	}
	if (IsDecideRelationPrefix(*result)) {
		result = ReadDecideRelationPrefix(*result).call->get();
	}
	return *result;
}

string WithoutEnclosingParentheses(const string &text) {
	if (text.size() < 2 || text.front() != '(' || text.back() != ')') {
		return text;
	}
	int depth = 0;
	for (idx_t i = 0; i + 1 < text.size(); i++) {
		depth += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
		if (depth == 0) {
			return text; // the opening parenthesis closes before the end
		}
	}
	return text.substr(1, text.size() - 2);
}

} // namespace duckdb
