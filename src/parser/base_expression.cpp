#include "duckdb/parser/base_expression.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/common/printer.hpp"

namespace duckdb {

void BaseExpression::Print() const {
	Printer::Print(ToString());
}

string BaseExpression::GetName() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return ToString();
	}
#endif
	// DECIDE tags ride in the alias and are never part of a name.
	auto name = StripDecideTags(alias);
	return !name.empty() ? name : ToString();
}

bool BaseExpression::Equals(const BaseExpression &other) const {
	if (expression_class != other.expression_class || type != other.type) {
		return false;
	}
	return true;
}

void BaseExpression::Verify() const {
}

} // namespace duckdb
