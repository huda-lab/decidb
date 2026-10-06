#include "duckdb/parser/expression/function_expression.hpp"

#include <utility>
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hash.hpp"

#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

FunctionExpression::FunctionExpression() : ParsedExpression(ExpressionType::FUNCTION, ExpressionClass::FUNCTION) {
}

FunctionExpression::FunctionExpression(string catalog, string schema, const string &function_name,
                                       vector<unique_ptr<ParsedExpression>> children_p,
                                       unique_ptr<ParsedExpression> filter, unique_ptr<OrderModifier> order_bys_p,
                                       bool distinct, bool is_operator, bool export_state_p)
    : ParsedExpression(ExpressionType::FUNCTION, ExpressionClass::FUNCTION), catalog(std::move(catalog)),
      schema(std::move(schema)), function_name(StringUtil::Lower(function_name)), is_operator(is_operator),
      children(std::move(children_p)), distinct(distinct), filter(std::move(filter)), order_bys(std::move(order_bys_p)),
      export_state(export_state_p) {
	D_ASSERT(!function_name.empty());
	if (!order_bys) {
		order_bys = make_uniq<OrderModifier>();
	}
}

FunctionExpression::FunctionExpression(const string &function_name, vector<unique_ptr<ParsedExpression>> children_p,
                                       unique_ptr<ParsedExpression> filter, unique_ptr<OrderModifier> order_bys,
                                       bool distinct, bool is_operator, bool export_state_p)
    : FunctionExpression(INVALID_CATALOG, INVALID_SCHEMA, function_name, std::move(children_p), std::move(filter),
                         std::move(order_bys), distinct, is_operator, export_state_p) {
}

//! Every child of a DECIDE marker after the one it marks, comma-separated: a PER or BY
//! key, or the relations qualifying a reducer.
static string DecideKeyToString(const FunctionExpression &marker) {
	string result;
	for (idx_t i = 1; i < marker.children.size(); i++) {
		result += (i > 1 ? ", " : "") + marker.children[i]->ToString();
	}
	return result;
}

//! A relation-qualified reducer is represented by a parser-only operator whose first
//! child is the real aggregate and whose remaining children are relation names. Render
//! the aggregate's complete DuckDB function surface, inserting the qualifier before
//! its arguments, rather than exposing the private marker as a function call.
static string QualifiedReducerToString(const FunctionExpression &wrapper) {
	if (wrapper.children.size() < 2 || wrapper.children[0]->GetExpressionClass() != ExpressionClass::FUNCTION) {
		throw InternalException("DECIDE qualified-reducer marker has an invalid parsed shape");
	}
	auto &aggregate = wrapper.children[0]->Cast<FunctionExpression>();
	string result;
	if (!aggregate.catalog.empty()) {
		result += KeywordHelper::WriteOptionallyQuoted(aggregate.catalog) + ".";
	}
	if (!aggregate.schema.empty()) {
		result += KeywordHelper::WriteOptionallyQuoted(aggregate.schema) + ".";
	}
	result += KeywordHelper::WriteOptionallyQuoted(aggregate.function_name) + "(";
	result += DecideKeyToString(wrapper) + ": ";
	if (aggregate.distinct) {
		result += "DISTINCT ";
	}
	for (idx_t i = 0; i < aggregate.children.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		auto &child = aggregate.children[i];
		result += child->GetAlias().empty()
		              ? child->ToString()
		              : StringUtil::Format("%s := %s", SQLIdentifier(child->GetAlias()), child->ToString());
	}
	if (aggregate.order_bys && !aggregate.order_bys->orders.empty()) {
		if (aggregate.children.empty()) {
			result += ") WITHIN GROUP (";
		}
		result += " ORDER BY ";
		for (idx_t i = 0; i < aggregate.order_bys->orders.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += aggregate.order_bys->orders[i].ToString();
		}
	}
	result += ")";
	if (aggregate.filter) {
		result += " FILTER (WHERE " + aggregate.filter->ToString() + ")";
	}
	if (aggregate.export_state) {
		result += " EXPORT_STATE";
	}
	return result;
}

//! `[WHEN condition] [PER key]: body`. PER is the outer marker and WHEN the inner one,
//! the order they apply in, so the pair is printed together -- WHEN first, as written.
static string DecideScopeToString(const FunctionExpression &scope) {
	const ParsedExpression *body = &scope;
	string per;
	if (scope.function_name == SCOPE_PER_TAG) {
		if (scope.children.empty()) {
			throw InternalException("DECIDE PER scope marker has no body");
		}
		auto key = DecideKeyToString(scope);
		per = key.empty() ? "PER ()" : "PER " + key;
		body = scope.children[0].get();
	}
	string when;
	if (body->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &filter = body->Cast<FunctionExpression>();
		if (filter.is_operator && filter.function_name == SCOPE_WHEN_TAG) {
			if (filter.children.size() != 2) {
				throw InternalException("DECIDE WHEN scope marker has %s children, expected 2",
				                        filter.children.size());
			}
			when = "WHEN " + filter.children[1]->ToString();
			body = filter.children[0].get();
		}
	}
	return when + (when.empty() || per.empty() ? "" : " ") + per + ": " + body->ToString();
}

string FunctionExpression::ToString() const {
	if (is_operator && (function_name == SCOPE_WHEN_TAG || function_name == SCOPE_PER_TAG)) {
		return DecideScopeToString(*this);
	}
	if (is_operator && function_name == REDUCER_BY_TAG) {
		if (children.empty()) {
			throw InternalException("DECIDE BY marker has no reducer");
		}
		return children[0]->ToString() + " BY (" + DecideKeyToString(*this) + ")";
	}
	if (is_operator && function_name == WHEN_CONSTRAINT_TAG) {
		if (children.size() != 2) {
			throw InternalException("DECIDE WHEN marker has %s children, expected 2", children.size());
		}
		return children[0]->ToString() + " WHEN " + children[1]->ToString();
	}
	if (is_operator && function_name == PER_CONSTRAINT_TAG) {
		if (children.size() < 2) {
			throw InternalException("DECIDE PER marker has %s children, expected at least 2", children.size());
		}
		auto key = DecideKeyToString(*this);
		return children[0]->ToString() + " PER " + (children.size() > 2 ? "(" + key + ")" : key);
	}
	if (is_operator && function_name == QUALIFIED_REDUCER_TAG) {
		return QualifiedReducerToString(*this);
	}
	return ToString<FunctionExpression, ParsedExpression>(*this, catalog, schema, function_name, is_operator, distinct,
	                                                      filter.get(), order_bys.get(), export_state, true);
}

bool FunctionExpression::Equal(const FunctionExpression &a, const FunctionExpression &b) {
	if (a.catalog != b.catalog || a.schema != b.schema || a.function_name != b.function_name ||
	    b.distinct != a.distinct) {
		return false;
	}
	if (b.children.size() != a.children.size()) {
		return false;
	}
	for (idx_t i = 0; i < a.children.size(); i++) {
		if (!a.children[i]->Equals(*b.children[i])) {
			return false;
		}
	}
	if (!ParsedExpression::Equals(a.filter, b.filter)) {
		return false;
	}
	if (!OrderModifier::Equals(a.order_bys, b.order_bys)) {
		return false;
	}
	if (a.export_state != b.export_state) {
		return false;
	}
	return true;
}

hash_t FunctionExpression::Hash() const {
	hash_t result = ParsedExpression::Hash();
	result = CombineHash(result, duckdb::Hash<const char *>(schema.c_str()));
	result = CombineHash(result, duckdb::Hash<const char *>(function_name.c_str()));
	result = CombineHash(result, duckdb::Hash<bool>(distinct));
	result = CombineHash(result, duckdb::Hash<bool>(export_state));
	return result;
}

unique_ptr<ParsedExpression> FunctionExpression::Copy() const {
	vector<unique_ptr<ParsedExpression>> copy_children;
	unique_ptr<ParsedExpression> filter_copy;
	copy_children.reserve(children.size());
	for (auto &child : children) {
		copy_children.push_back(child->Copy());
	}
	if (filter) {
		filter_copy = filter->Copy();
	}
	auto order_copy = order_bys ? unique_ptr_cast<ResultModifier, OrderModifier>(order_bys->Copy()) : nullptr;
	auto copy =
	    make_uniq<FunctionExpression>(catalog, schema, function_name, std::move(copy_children), std::move(filter_copy),
	                                  std::move(order_copy), distinct, is_operator, export_state);
	copy->CopyProperties(*this);
	return std::move(copy);
}

void FunctionExpression::Verify() const {
	D_ASSERT(!function_name.empty());
}

bool FunctionExpression::IsLambdaFunction() const {
	// Ignore the ->> operator (JSON extension).
	if (function_name == "->>") {
		return false;
	}
	// Check the children for lambda expressions.
	for (auto &child : children) {
		if (child->GetExpressionClass() == ExpressionClass::LAMBDA) {
			return true;
		}
	}
	return false;
}

} // namespace duckdb
