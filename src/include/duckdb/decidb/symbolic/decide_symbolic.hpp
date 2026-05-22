//===----------------------------------------------------------------------===//
//                         DecidB
//
// decidb/symbolic/decide_symbolic.hpp
//
// Symbolic Translation Layer for DECIDE Expressions
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

#include "duckdb/common/unordered_map.hpp"

// Forward declare Symbolic from SymbolicC++ (global namespace)
class Symbolic;

namespace duckdb {

//! Context for symbolic translation - tracks DECIDE variables and row-varying columns
struct SymbolicTranslationContext {
    //! Map of DECIDE variable names to their indices
    const case_insensitive_map_t<idx_t> &decide_variables;
    
    //! Map of placeholder names to original subquery expressions
    unordered_map<string, unique_ptr<ParsedExpression>> subquery_map;

    //! Map of placeholder names to original ABS(...) expressions (opaque through normalization)
    unordered_map<string, unique_ptr<ParsedExpression>> abs_map;

    //! Constructor
    explicit SymbolicTranslationContext(const case_insensitive_map_t<idx_t> &vars)
        : decide_variables(vars) {}
};

//! Converts a Symbolic object back to a ParsedExpression (preferred)
unique_ptr<ParsedExpression> FromSymbolic(const ::Symbolic &symbolic, SymbolicTranslationContext &ctx);

//! Helper: Check if an expression is a DECIDE variable
bool IsDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

//! Normalize constraints: factor numeric scalars from SUM products on LHS and
//! adjust RHS/scalar accordingly; recurse through AND conjunctions.
unique_ptr<ParsedExpression> NormalizeDecideConstraints(const ParsedExpression &expr,
                                                        const case_insensitive_map_t<idx_t> &decide_variables);

//! Normalize objective: rewrite SUM inner as x * (row_expr) and combine numeric
//! constants inside the inner product; does not change overall scaling.
//!
//! Additive constant offsets on the objective body (`MAXIMIZE SUM(x) + 3`,
//! `MAXIMIZE (SUM(x) WHEN c) + 3`, `MAXIMIZE 2 * SUM(x) + SUM(y) - 5`) are
//! peeled from the body because they don't affect `argmax`/`argmin`. The
//! peeled offset is returned via `out_constant_offset` so the caller can
//! stash it on `LogicalDecide` for later retrieval (e.g. if/when DecidB
//! surfaces the objective *value* to users, the offset must be added back).
//! If no offset is peeled, the out parameter is left at 0.0.
unique_ptr<ParsedExpression> NormalizeDecideObjective(const ParsedExpression &expr,
                                                      const case_insensitive_map_t<idx_t> &decide_variables,
                                                      double &out_constant_offset);

//! Produce a Graphviz DOT representation of a ParsedExpression tree
string ExpressionToDot(const ParsedExpression &expr);

} // namespace duckdb

