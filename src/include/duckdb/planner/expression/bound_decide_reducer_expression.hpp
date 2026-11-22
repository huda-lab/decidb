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

//! The order of `norm(e, p)` and, for `norm(e, 0, M)`, its bound.
struct DecideNorm {
	enum class Order : uint8_t { NONE, L0, L1, L2, INF };

	Order order = Order::NONE;
	//! The M of an L0 norm as written; 0 for the bound stage 06 derives.
	double bound = 0;

	explicit operator bool() const {
		return order != Order::NONE;
	}
	bool operator==(const DecideNorm &other) const {
		return order == other.order && bound == other.bound;
	}
	//! Whether the norm counts the nonzero values of its body.
	bool IsCount() const {
		return order == Order::L0;
	}
	//! The arguments after the body as the user writes them: `1`, `2`, `'inf'` (a string literal: `inf` alone names
	//! a column), `0`, or `0, <M>`.
	string Written() const;
};

//! A reducer of a DECIDE clause (`sum`, `avg`, `min`, `max`, `count`, `norm`), and the aggregate a frame binds to
//! (`first(e)` for a point, the frame's reducer for a range). It is laid out as DuckDB's BoundWindowExpression is: the
//! bound aggregate function with its bind data, its arguments, and its rows, its keys and what the rewrites record on
//! it as fields. It is no SQL aggregate: no LogicalAggregate computes it and DuckDB's executor never reaches it. The
//! tree lowers it, and what DuckDB runs is the aggregate it names (ToAggregate).
class BoundDecideReducerExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_DECIDE_REDUCER;

public:
	BoundDecideReducerExpression(LogicalType return_type, unique_ptr<AggregateFunction> function,
	                             vector<unique_ptr<Expression>> children, unique_ptr<Expression> filter,
	                             unique_ptr<FunctionData> bind_info);
	//! The reducer `aggregate` (a BoundAggregateExpression) is over a clause's rows.
	explicit BoundDecideReducerExpression(unique_ptr<Expression> aggregate);

	//! The bound overload; null for a norm no definition has replaced, which `norm` names. Canonicalization replaces
	//! the orders 1, 2 and 'inf', and an L0 norm of data, by their definitions; DecideOptimizer::RewriteNorm lowers an
	//! L0 norm of a decision.
	unique_ptr<AggregateFunction> function;
	//! What it reads, in the function's order: none for COUNT(*), the value and its key for a collated MIN or MAX
	vector<unique_ptr<Expression>> children;
	//! The bind info
	unique_ptr<FunctionData> bind_info;
	//! The rows it reads: its `when` and the NULL tests of its members (BoundDecideNullTestExpression); null for every
	//! row
	unique_ptr<Expression> filter;

	//! Its `by` key, an index into LogicalDecide::keys: the rows it reads at a class are the kept rows sharing the
	//! class's key value. INVALID_INDEX for `by ()`, every kept row.
	idx_t by = DConstants::INVALID_INDEX;
	//! The keyed scope of its own `per`, `sum(per K: e)`: an index into LogicalDecide::keyed_scopes, whose entry
	//! supplies the classes it takes one value from. INVALID_INDEX for none.
	idx_t per = DConstants::INVALID_INDEX;
	//! The norm it is (no `function`), or the norm its definition was written as (`SUM(ABS(e))` for 1,
	//! `SUM(POWER(e, 2))` for 2, `MAX(ABS(e))` for 'inf'): the tree means what it says, and this lets the canonical
	//! text read as written, until RewriteNorm clears it.
	DecideNorm norm;
	//! The name it was written under when the binder renamed the call to its primary (`mean` for `avg`). Display
	//! only: two reducers that differ in it are equal.
	string written_name;
	//! It is the SUM an AVG became (DecideOptimizer's AVG rewrite): its terms scale by its row count.
	bool count_scale = false;
	//! It is an objective's reducer over a decision reducer: the canonicalizer sets it once, and
	//! DecideOptimizer::RewriteScopedReducers replaces the reducer by a helper decision.
	bool holds_reducer = false;
	//! The nest it was inlined in place of (InlineSumOfGroupSums), an index into LogicalDecide::retained_expressions,
	//! which the tree lowers for it. INVALID_INDEX for none.
	idx_t retained = DConstants::INVALID_INDEX;

public:
	//! The SQL aggregate it names, over the same arguments and rows.
	unique_ptr<Expression> ToAggregate() const;
	//! Takes the facts of `other`, the reducer it is built in place of: its keys, its norm, its written name and what
	//! the rewrites recorded.
	void CopyFacts(const BoundDecideReducerExpression &other);
	//! Whether the two agree on those facts, the written name apart. Equals leaves them out, as it does an alias.
	bool FactsEqual(const BoundDecideReducerExpression &other) const;
	//! The name it was written under.
	const string &WrittenName() const {
		return written_name.empty() ? function->name : written_name;
	}

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
