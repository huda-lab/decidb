#include "duckdb/parser/expression/decide_reducer_expression.hpp"

#include "duckdb/common/types/hash.hpp"

namespace duckdb {

DecideReducerExpression::DecideReducerExpression()
    : ParsedExpression(ExpressionType::DECIDE_REDUCER, ExpressionClass::DECIDE_REDUCER) {
}

string DecideReducerExpression::ToString() const {
	auto result = call->ToString();
	if (!scope.IsEmpty()) {
		// The scope stands in front of the first argument, after the call's opening parenthesis.
		result.insert(result.find('(') + 1, scope.ToString() + ": ");
	}
	return has_by ? result + " BY (" + DecideKeyToString(by) + ")" : result;
}

bool DecideReducerExpression::Equal(const DecideReducerExpression &a, const DecideReducerExpression &b) {
	return a.has_by == b.has_by && ParsedExpression::Equals(a.call, b.call) && a.scope.Equals(b.scope) &&
	       ParsedExpression::ListEquals(a.by, b.by);
}

hash_t DecideReducerExpression::Hash() const {
	// ParsedExpression::Hash covers the call and every scope and key expression (EnumerateChildren).
	hash_t result = ParsedExpression::Hash();
	result = CombineHash(result, duckdb::Hash<uint8_t>(static_cast<uint8_t>(scope.per)));
	return CombineHash(result, duckdb::Hash<bool>(has_by));
}

unique_ptr<ParsedExpression> DecideReducerExpression::Copy() const {
	auto copy = make_uniq<DecideReducerExpression>();
	copy->CopyProperties(*this);
	copy->call = call->Copy();
	copy->scope = scope.Copy();
	copy->has_by = has_by;
	for (auto &element : by) {
		copy->by.push_back(element->Copy());
	}
	return std::move(copy);
}

} // namespace duckdb
