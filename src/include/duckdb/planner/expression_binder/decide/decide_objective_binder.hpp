//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/decide/decide_objective_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/common/enums/decide.hpp"

namespace duckdb {

//! The DecideObjective binder is responsible for binding an expression within the [MAXIMIZE|MINIMIZE] clause of a SQL statement
class DecideObjectiveBinder : public DecideBinder {
public:
    DecideObjectiveBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables,
                          const case_insensitive_set_t &scalar_variables = case_insensitive_set_t(),
                          optional_ptr<DecideQualifierContext> qualifier_context = nullptr);
    DecideSense decide_sense = DecideSense::MINIMIZE;

protected:
    BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression = false) override;
    //! The dispatch itself; `BindExpression` wraps it to stamp the source location.
    BindResult BindExpressionInternal(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression);

private:
    DecideExpression GetExpressionType(ParsedExpression &expr, string &error_msg) override;
    //! Binds `[when c] [per ()]: objective`: `per ()` is the objective's only key and adds
    //! nothing; `when c` becomes the WHEN wrapper.
    BindResult BindScopedObjective(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
    //! Binds the two-level form `outer(per k: inner(e) by (k))`: the outer reducer's PER
    //! wrapper over the inner reducer; NULL keys form a class.
    BindResult BindTwoLevelObjective(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
};

} // namespace duckdb