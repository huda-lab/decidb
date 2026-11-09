//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/rule/case_simplification.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/rule.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"

namespace duckdb {

// The Case Simplification rule rewrites cases with a constant check (i.e. [CASE WHEN 1=1 THEN x ELSE y END] => x)
class CaseSimplificationRule : public Rule {
public:
	explicit CaseSimplificationRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;

	//! Folds the CASE's constant WHEN checks in place: a false or NULL one is erased, a true one becomes the ELSE and
	//! ends the checks. Returns the ELSE when no check is left, and nullptr otherwise. A check whose evaluation fails
	//! is kept, to raise at run time. Statistics propagation calls this too, after it has turned a check into a
	//! constant.
	static unique_ptr<Expression> Simplify(ClientContext &context, BoundCaseExpression &root);
};

} // namespace duckdb
