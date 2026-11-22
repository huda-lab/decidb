#include "duckdb/parser/decide/parsed_decide.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

string DecideKeyToString(const vector<unique_ptr<ParsedExpression>> &key) {
	return StringUtil::Join(key, key.size(), ", ",
	                        [](const unique_ptr<ParsedExpression> &element) { return element->ToString(); });
}

//===--------------------------------------------------------------------===//
// DecideScope
//===--------------------------------------------------------------------===//
string DecideScope::ToString() const {
	string result = when ? "WHEN " + when->ToString() : string();
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
		return result + "PER " + DecideKeyToString(key);
	}
}

DecideScope DecideScope::Copy() const {
	DecideScope result;
	result.when = when ? when->Copy() : nullptr;
	result.per = per;
	for (auto &element : key) {
		result.key.push_back(element->Copy());
	}
	result.location = location;
	return result;
}

bool DecideScope::Equals(const DecideScope &other) const {
	return per == other.per && ParsedExpression::Equals(when, other.when) &&
	       ParsedExpression::ListEquals(key, other.key);
}

void DecideScope::EnumerateExpressions(const std::function<void(unique_ptr<ParsedExpression> &child)> &callback) {
	if (when) {
		callback(when);
	}
	for (auto &element : key) {
		callback(element);
	}
}

//===--------------------------------------------------------------------===//
// DecideDeclaration
//===--------------------------------------------------------------------===//
string DecideDeclaration::ToString() const {
	auto prefix = scope.IsEmpty() ? string() : scope.ToString() + ": ";
	return prefix + KeywordHelper::WriteOptionallyQuoted(name) + "(" + EnumUtil::ToString(type) + ")";
}

DecideDeclaration DecideDeclaration::Copy() const {
	DecideDeclaration result;
	result.name = name;
	result.type = type;
	result.scope = scope.Copy();
	result.location = location;
	return result;
}

bool DecideDeclaration::Equals(const DecideDeclaration &other) const {
	return name == other.name && type == other.type && scope.Equals(other.scope);
}

//===--------------------------------------------------------------------===//
// DecideClause
//===--------------------------------------------------------------------===//
string DecideClause::ToString() const {
	return (scope.IsEmpty() ? string() : scope.ToString() + ": ") + body->ToString();
}

DecideClause DecideClause::Copy() const {
	DecideClause result;
	result.scope = scope.Copy();
	result.body = body ? body->Copy() : nullptr;
	return result;
}

bool DecideClause::Equals(const DecideClause &other) const {
	return scope.Equals(other.scope) && ParsedExpression::Equals(body, other.body);
}

//===--------------------------------------------------------------------===//
// ParsedDecide
//===--------------------------------------------------------------------===//
string ParsedDecide::ToString() const {
	auto result = "DECIDE " + StringUtil::Join(variables, variables.size(), ", ",
	                                           [](const DecideDeclaration &entry) { return entry.ToString(); });
	if (text.HasText()) {
		// the tail as written, so a statement parsed from this string keeps the text the user wrote
		return result + " " + text.Tail();
	}
	result += " SUCH THAT " + StringUtil::Join(constraints, constraints.size(), " AND ",
	                                           [](const DecideClause &clause) { return clause.ToString(); });
	if (objective.body) {
		result += sense == DecideSense::MAXIMIZE ? " MAXIMIZE " : " MINIMIZE ";
		result += objective.ToString();
	}
	return result;
}

unique_ptr<ParsedDecide> ParsedDecide::Copy() const {
	auto result = make_uniq<ParsedDecide>();
	for (auto &variable : variables) {
		result->variables.push_back(variable.Copy());
	}
	for (auto &constraint : constraints) {
		result->constraints.push_back(constraint.Copy());
	}
	result->objective = objective.Copy();
	result->sense = sense;
	result->text = text;
	return result;
}

bool ParsedDecide::Equals(const ParsedDecide &other) const {
	if (variables.size() != other.variables.size() || constraints.size() != other.constraints.size() ||
	    sense != other.sense || !objective.Equals(other.objective)) {
		return false;
	}
	for (idx_t i = 0; i < variables.size(); i++) {
		if (!variables[i].Equals(other.variables[i])) {
			return false;
		}
	}
	for (idx_t i = 0; i < constraints.size(); i++) {
		if (!constraints[i].Equals(other.constraints[i])) {
			return false;
		}
	}
	return true;
}

void ParsedDecide::EnumerateExpressions(const std::function<void(unique_ptr<ParsedExpression> &child)> &callback) {
	for (auto &variable : variables) {
		variable.scope.EnumerateExpressions(callback);
	}
	for (auto &constraint : constraints) {
		constraint.scope.EnumerateExpressions(callback);
		callback(constraint.body);
	}
	if (objective.body) {
		objective.scope.EnumerateExpressions(callback);
		callback(objective.body);
	}
}

} // namespace duckdb
