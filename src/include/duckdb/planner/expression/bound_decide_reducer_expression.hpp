//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/expression/bound_decide_reducer_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! A reducer of a DECIDE clause (`sum`, `avg`, `min`, `max`, `count`, `norm`), and the aggregate a frame binds to
//! (`first(e)` for a point, the frame's reducer for a range). It is laid out as DuckDB's BoundWindowExpression is: the
//! bound aggregate function with its bind data, its arguments, and its rows as a field. It is no SQL aggregate: no
//! LogicalAggregate computes it and DuckDB's executor never reaches it. The tree lowers it, and what DuckDB runs is
//! the aggregate it names (ToAggregate).
class BoundDecideReducerExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_DECIDE_REDUCER;

public:
	BoundDecideReducerExpression(AggregateFunction function, vector<unique_ptr<Expression>> children,
	                             unique_ptr<Expression> filter, unique_ptr<FunctionData> bind_info);
	//! The reducer `aggregate` (a BoundAggregateExpression) is over a clause's rows.
	explicit BoundDecideReducerExpression(unique_ptr<Expression> aggregate);

	//! The bound overload
	AggregateFunction function;
	//! What it reads, in the function's order: none for COUNT(*), the value and its key for a collated MIN or MAX
	vector<unique_ptr<Expression>> children;
	//! The bind info
	unique_ptr<FunctionData> bind_info;
	//! The rows it reads: its `when` and the NULL tests of its members (BoundDecideNullTestExpression); null for every
	//! row
	unique_ptr<Expression> filter;

public:
	//! The SQL aggregate it names, over the same arguments and rows.
	unique_ptr<Expression> ToAggregate() const;

	bool IsAggregate() const override {
		return true;
	}
	bool IsFoldable() const override {
		return false;
	}
	bool PropagatesNullValues() const override;

	string ToString() const override;

	hash_t Hash() const override;
	bool Equals(const BaseExpression &other) const override;

	unique_ptr<Expression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);
};
} // namespace duckdb
