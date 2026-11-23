//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_decide_frame_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! A frame of a DECIDE clause, `at(F: e)` or `agg(from F1 to F2: e)`: the value its payload takes at the rows the
//! frame selects from an anchor row (language/guide.md §5.6). It names its frame by index into BoundDecide::frames,
//! the record that holds what the frame is (its ends, its timeline, its `else`, a range's reducer), and holds what
//! the frame reads, so every visitor of a clause reaches what the frame reads. It is no reducer: it has no rows, keys
//! or function of its own, and two copies of it cannot disagree about the frame. DuckDB's executor never reaches it;
//! the tree lowers it to an O node.
class BoundDecideFrameExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_DECIDE_FRAME;

public:
	BoundDecideFrameExpression(LogicalType return_type, idx_t frame, vector<unique_ptr<Expression>> children);

	//! Its entry of BoundDecide::frames
	idx_t frame;
	//! What it reads at each row the frame selects: the payload, and after it the key a collated MIN or MAX range
	//! compares by, in the order of the range's reducer (DecideFrameSpec::reduce)
	vector<unique_ptr<Expression>> children;

public:
	//! Its value is read at other rows than the one it stands on, so no pass may treat it as row-local.
	bool IsAggregate() const override {
		return true;
	}
	bool IsFoldable() const override {
		return false;
	}
	//! A missing position reads the frame's `else`, or no value, whatever the payload is at the anchor row.
	bool PropagatesNullValues() const override {
		return false;
	}

	string ToString() const override;

	hash_t Hash() const override;
	bool Equals(const BaseExpression &other) const override;

	unique_ptr<Expression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);
};
} // namespace duckdb
