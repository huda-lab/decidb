#include "duckdb/parser/decide/decide_declaration.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

const char *DecideDomainName(DecideDomain domain) {
	switch (domain) {
	case DecideDomain::INT:
		return "INT";
	case DecideDomain::REAL:
		return "REAL";
	case DecideDomain::BOOL:
		return "BOOL";
	case DecideDomain::SEMIREAL:
		return "SEMIREAL";
	case DecideDomain::SEMIINT:
		return "SEMIINT";
	case DecideDomain::TEXT:
		return "TEXT";
	}
	return "INT";
}

string DecideScopePrefixToString(DecideScopeKind kind, const vector<unique_ptr<ParsedExpression>> &key) {
	switch (kind) {
	case DecideScopeKind::ROW:
		return "";
	case DecideScopeKind::GLOBAL:
		return "PER (): ";
	case DecideScopeKind::KEY:
		return "PER " +
		       StringUtil::Join(key, key.size(), ", ",
		                        [](const unique_ptr<ParsedExpression> &elem) { return elem->ToString(); }) +
		       ": ";
	}
	return "";
}

DecideDeclaration DecideDeclaration::Copy() const {
	DecideDeclaration result;
	result.name = name;
	result.scope_kind = scope_kind;
	for (auto &elem : scope_key) {
		result.scope_key.push_back(elem->Copy());
	}
	result.domain = domain;
	result.text_values = text_values;
	result.lower_bound = lower_bound ? lower_bound->Copy() : nullptr;
	result.upper_bound = upper_bound ? upper_bound->Copy() : nullptr;
	return result;
}

bool DecideDeclaration::Equals(const DecideDeclaration &other) const {
	return StringUtil::CIEquals(name, other.name) && scope_kind == other.scope_kind &&
	       ParsedExpression::ListEquals(scope_key, other.scope_key) && domain == other.domain &&
	       text_values == other.text_values && ParsedExpression::Equals(lower_bound, other.lower_bound) &&
	       ParsedExpression::Equals(upper_bound, other.upper_bound);
}

string DecideDeclaration::ToString() const {
	string result = DecideScopePrefixToString(scope_kind, scope_key);
	result += KeywordHelper::WriteOptionallyQuoted(name) + "(" + DecideDomainName(domain);
	if (domain == DecideDomain::TEXT) {
		result += " IN [" +
		          StringUtil::Join(text_values, text_values.size(), ", ",
		                           [](const string &value) { return "'" + StringUtil::Replace(value, "'", "''") + "'"; }) +
		          "]";
	}
	result += ")";
	if (lower_bound && upper_bound) {
		result += " BETWEEN " + lower_bound->ToString() + " AND " + upper_bound->ToString();
	} else if (lower_bound) {
		result += " >= " + lower_bound->ToString();
	} else if (upper_bound) {
		result += " <= " + upper_bound->ToString();
	}
	return result;
}

DecideObjectiveClause DecideObjectiveClause::Copy() const {
	return DecideObjectiveClause(sense, expression ? expression->Copy() : nullptr);
}

bool DecideObjectiveClause::Equals(const DecideObjectiveClause &other) const {
	return sense == other.sense && ParsedExpression::Equals(expression, other.expression);
}

string DecideObjectiveClause::ToString() const {
	string result = sense == DecideSense::MAXIMIZE ? "MAXIMIZE " : "MINIMIZE ";
	return result + (expression ? expression->ToString() : string());
}

} // namespace duckdb
