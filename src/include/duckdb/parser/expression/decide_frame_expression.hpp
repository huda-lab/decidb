//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/parser/expression/decide_frame_expression.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

//! A frame selector inside a DECIDE clause (syntax_reference.md §5.6): it reads a
//! payload at a position of the anchor row's timeline. A point frame selects one
//! position, `at(previous else 0: x) over (t.period within t.product)`; a range frame
//! reduces the payload over several, `sum(from 2 previous to previous: x) over (...)`.
//! The timeline orders the rows by `order` inside each partition of `within`.
class DecideFrameExpression : public ParsedExpression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::DECIDE_FRAME;

public:
	DecideFrameExpression();

	//! The reducer of a range frame (`sum`), lower-case; empty for a point frame
	string reducer;
	//! The point, or the range's first end: an anchor and its k (k previous, k next)
	DecideFrameAnchor from = DecideFrameAnchor::FIRST;
	int64_t from_k = 1;
	//! The range's last end
	DecideFrameAnchor to = DecideFrameAnchor::FIRST;
	int64_t to_k = 1;
	//! A range's step, `every d`
	int64_t every = 1;
	//! `all`: a range with a missing position selects nothing
	bool all = false;
	//! The expression read at the selected row
	unique_ptr<ParsedExpression> payload;
	//! `else v`, read at the anchor row for a missing position; null when unwritten
	unique_ptr<ParsedExpression> fallback;
	//! The timeline's order key, `over (S ...)`
	unique_ptr<ParsedExpression> order;
	bool descending = false;
	bool cyclic = false;
	//! The timeline's partition key, `within K`: columns and relations, empty for one
	//! timeline over the rows
	vector<unique_ptr<ParsedExpression>> within;

public:
	bool IsRange() const {
		return !reducer.empty();
	}
	//! `previous`, `3 next`: one position as written, k omitted when it is 1
	static string PositionToString(DecideFrameAnchor anchor, int64_t k);

	string ToString() const override;
	static bool Equal(const DecideFrameExpression &a, const DecideFrameExpression &b);
	hash_t Hash() const override;
	unique_ptr<ParsedExpression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
