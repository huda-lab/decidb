//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/decide_objective_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_binder/decide_binder.hpp"
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

private:
    DecideExpression GetExpressionType(ParsedExpression &expr, string &error_msg) override;
    bool binding_when_condition = false;
};

} // namespace duckdb