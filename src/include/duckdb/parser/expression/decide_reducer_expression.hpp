//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/expression/decide_reducer_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/decide/parsed_decide.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

//! A reducer written with DECIDE syntax (language/guide.md §5): a scope in front of its first argument,
//! `sum(when c per K: e)`, a `by` after it, `sum(e) by (K)`, or both. A plain `sum(e)` stays a FunctionExpression.
class DecideReducerExpression : public ParsedExpression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::DECIDE_REDUCER;

public:
	DecideReducerExpression();

	//! The call as SQL parses it, without the scope
	unique_ptr<ParsedExpression> call;
	//! The scope in front of the call's first argument
	DecideScope scope;
	//! Whether a `by` is written; `by ()` has no key
	bool has_by = false;
	vector<unique_ptr<ParsedExpression>> by;

public:
	string ToString() const override;
	static bool Equal(const DecideReducerExpression &a, const DecideReducerExpression &b);
	hash_t Hash() const override;
	unique_ptr<ParsedExpression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
