//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/expression/bound_decide_null_guard_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! A coefficient read as 0 on the rows a SUM skips: `COALESCE(c, 0)` on the solver path, `CASE WHEN valid THEN c ELSE 0
//! END` in a direct rule's reading. The query never wrote the guard, so a render prints the value it protects, and the
//! value decides whether the coefficient is a constant. A Decorator over the guard, as BoundDecideNullTestExpression is
//! over a test: it executes as the guard and travels wherever the coefficient is copied.
class BoundDecideNullGuardExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_DECIDE_NULL_GUARD;

public:
	explicit BoundDecideNullGuardExpression(unique_ptr<Expression> guard);

	//! The COALESCE or the CASE.
	unique_ptr<Expression> guard;

public:
	//! The value the guard protects: the first operand of the COALESCE, the THEN of the CASE.
	const Expression &Value() const;

	string ToString() const override;

	bool Equals(const BaseExpression &other) const override;

	unique_ptr<Expression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);
};
} // namespace duckdb
