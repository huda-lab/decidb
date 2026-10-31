//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/decide/decide_constraints_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/common/enums/decide.hpp"

namespace duckdb {

//! The DecideConstraints binder is responsible for binding an expression within the SUCH THAT clause of a SQL statement
class DecideConstraintsBinder : public DecideBinder {
public:
    DecideConstraintsBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables,
                            const case_insensitive_set_t &scalar_variables = case_insensitive_set_t(),
                            optional_ptr<DecideQualifierContext> qualifier_context = nullptr);

protected:
    BindResult BindNode(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) override;

private:
    DecideExpression GetExpressionType(ParsedExpression &expr, string &error_msg) override;
    BindResult BindComparison(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
    BindResult BindOperator(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
    BindResult BindBetween(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
    BindResult BindConjunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
    //! Binds `[when c] [per K]: constraint`: the constraint wrapped in a WHEN wrapper for
    //! `c` and a PER_CLASS_TAG wrapper for `K` (no columns for `per ()`). A BETWEEN binds to
    //! two comparisons, each wrapped in the scope.
    BindResult BindScopedConstraint(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
};

} // namespace duckdb
