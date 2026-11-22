//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/expression/bound_decide_null_test_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! A NULL test a DECIDE reducer's rows gain beside its written `when` (DecideCanonicalizer::SkipNullData): a conjunct
//! the query never wrote, so no render prints it, which drops the reducer's own members only. A Decorator over the
//! test, as PostgreSQL's RestrictInfo is over a conjunct: it travels in the conjunction wherever the reducer's rows
//! are copied, executes as the test, and the tree splits it from the written conditions when it is built
//! (DecideNode::member_tests). The test around a recorded reducer's value is one too (RhsBoundRewriter::InKey): it
//! says which rows are in a group of the reducer's key. Either way the test only settles membership, so what it reads
//! is no field of a signature.
class BoundDecideNullTestExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_DECIDE_NULL_TEST;

public:
	explicit BoundDecideNullTestExpression(unique_ptr<Expression> test);

	//! `value IS NOT NULL`, or TRUE once the statistics prove it (StatisticsPropagator::FoldSkipNullTests).
	unique_ptr<Expression> test;

public:
	//! Whether the test keeps every row: the statistics proved it true.
	bool KeepsEveryRow() const;

	bool IsFoldable() const override;
	string ToString() const override;

	hash_t Hash() const override;
	bool Equals(const BaseExpression &other) const override;

	unique_ptr<Expression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);
};
} // namespace duckdb
