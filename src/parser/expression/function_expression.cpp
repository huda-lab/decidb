#include "duckdb/parser/expression/function_expression.hpp"

#include <utility>
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/parser/decide/decide_frame_spec.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/keyword_helper.hpp"
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

//! DECIDE spellings. The parser encodes the DeciQL constructs as tagged operator
//! FunctionExpressions (common/enums/decide.hpp); these render them back in the
//! grammar's own form so a parsed statement round-trips through ToString().

static string DecideKeyListToString(const vector<unique_ptr<ParsedExpression>> &children, idx_t first) {
	string result;
	for (idx_t i = first; i < children.size(); i++) {
		if (i > first) {
			result += ", ";
		}
		result += children[i]->ToString();
	}
	return result;
}

//! `WHEN c PER k IF b: body` from any nesting of the three prefix wrappers.
static string DecidePrefixedToString(const FunctionExpression &wrapper) {
	string filter, scope, guard;
	bool has_scope = false;
	const ParsedExpression *current = &wrapper;
	while (current->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = current->Cast<FunctionExpression>();
		if (!func.is_operator || func.children.empty()) {
			break;
		}
		if (func.function_name == WHEN_CONSTRAINT_TAG && func.children.size() == 2) {
			filter = func.children[1]->ToString();
		} else if (func.function_name == IF_CONSTRAINT_TAG && func.children.size() == 2) {
			guard = func.children[1]->ToString();
		} else if (func.function_name == PER_CONSTRAINT_TAG) {
			has_scope = true;
			scope = func.children.size() == 1 ? "()" : DecideKeyListToString(func.children, 1);
		} else {
			break;
		}
		current = func.children[0].get();
	}
	vector<string> parts;
	if (!filter.empty()) {
		parts.push_back("WHEN " + filter);
	}
	if (has_scope) {
		parts.push_back("PER " + scope);
	}
	if (!guard.empty()) {
		parts.push_back("IF " + guard);
	}
	return StringUtil::Join(parts, " ") + ": " + current->ToString();
}

//! `agg(WHEN f PER k: body) BY (keys)` from BY(QUALIFIED(agg)) in either nesting.
static string DecideReducerToString(const FunctionExpression &wrapper) {
	string by_keys, per_scope;
	bool has_by = false, has_per = false;
	const ParsedExpression *current = &wrapper;
	while (current->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = current->Cast<FunctionExpression>();
		if (!func.is_operator || func.children.empty()) {
			break;
		}
		if (func.function_name == REDUCER_BY_TAG) {
			has_by = true;
			by_keys = DecideKeyListToString(func.children, 1);
		} else if (func.function_name == QUALIFIED_REDUCER_TAG) {
			has_per = true;
			per_scope = func.children.size() == 1 ? "()" : DecideKeyListToString(func.children, 1);
		} else {
			break;
		}
		current = func.children[0].get();
	}
	if (current->GetExpressionClass() != ExpressionClass::FUNCTION) {
		throw InternalException("DECIDE reducer marker does not wrap a function call");
	}
	auto &aggregate = current->Cast<FunctionExpression>();
	string result;
	if (!aggregate.catalog.empty()) {
		result += KeywordHelper::WriteOptionallyQuoted(aggregate.catalog) + ".";
	}
	if (!aggregate.schema.empty()) {
		result += KeywordHelper::WriteOptionallyQuoted(aggregate.schema) + ".";
	}
	result += KeywordHelper::WriteOptionallyQuoted(aggregate.function_name) + "(";
	vector<string> prefix;
	if (aggregate.filter) {
		prefix.push_back("WHEN " + aggregate.filter->ToString());
	}
	if (has_per) {
		prefix.push_back("PER " + per_scope);
	}
	if (!prefix.empty()) {
		result += StringUtil::Join(prefix, " ") + ": ";
	}
	result += DecideKeyListToString(aggregate.children, 0) + ")";
	if (has_by) {
		result += " BY (" + by_keys + ")";
	}
	return result;
}

//! `AT(sel [ELSE v]: e) OVER (key [DESC] [CYCLIC] [WITHIN p])` and the range form.
static string DecideFrameToString(const FunctionExpression &wrapper) {
	if (wrapper.children.size() < 4 || wrapper.children[1]->GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw InternalException("DECIDE frame marker has an invalid parsed shape");
	}
	auto spec = DecideFrameSpec::Decode(wrapper.children[1]->Cast<ConstantExpression>().value.ToString());
	string result;
	if (spec.is_range) {
		result = KeywordHelper::WriteOptionallyQuoted(spec.aggregate) + "(FROM " + spec.from_selector.ToString() +
		         " TO " + spec.to_selector.ToString();
		if (spec.every != 1) {
			result += " EVERY " + to_string(spec.every);
		}
	} else {
		result = "AT(" + spec.from_selector.ToString();
	}
	switch (spec.policy) {
	case DecideFramePolicy::ELSE_NULL:
		break;
	case DecideFramePolicy::ELSE_VALUE:
		result += " ELSE " + wrapper.children[3]->ToString();
		break;
	case DecideFramePolicy::ALL:
		result += " ALL";
		break;
	}
	result += ": " + wrapper.children[0]->ToString() + ") OVER (" + wrapper.children[2]->ToString();
	if (spec.descending) {
		result += " DESC";
	}
	if (spec.cyclic) {
		result += " CYCLIC";
	}
	if (spec.has_within) {
		result += " WITHIN " + (wrapper.children.size() == 4 ? string("()") : DecideKeyListToString(wrapper.children, 4));
	}
	return result + ")";
}

string FunctionExpression::ToString() const {
	if (is_operator) {
		if (function_name == WHEN_CONSTRAINT_TAG || function_name == PER_CONSTRAINT_TAG ||
		    function_name == IF_CONSTRAINT_TAG) {
			return DecidePrefixedToString(*this);
		}
		if (function_name == QUALIFIED_REDUCER_TAG || function_name == REDUCER_BY_TAG) {
			return DecideReducerToString(*this);
		}
		if (function_name == FRAME_TAG) {
			return DecideFrameToString(*this);
		}
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
