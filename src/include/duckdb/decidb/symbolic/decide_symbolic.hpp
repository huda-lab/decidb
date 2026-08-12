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

    //! Map of placeholder names to original data-only subexpressions that use an
    //! operator the symbolic algebra doesn't model (e.g. `(id * 7) % 97`). Because
    //! they reference no DECIDE variable they are per-row constant coefficients;
    //! we keep them opaque through normalization (like ABS/subqueries) and restore
    //! the original expression in FromSymbolic so the physical layer evaluates them
    //! as ordinary data. Unlike abs_map these are data-side, not decide-side.
    unordered_map<string, unique_ptr<ParsedExpression>> data_map;

    //! Map of qualified column paths (`t1.w`, lowercased) to the original column
    //! reference. A `Symbolic` symbol carries only a name, so a qualified column
    //! written in a constraint or objective would otherwise come back out of
    //! FromSymbolic as a bare `ColumnRefExpression(GetColumnName())` — losing the
    //! qualifier and becoming ambiguous when two tables in the FROM share a column
    //! name. Keying the symbol by full path keeps distinct columns distinct in the
    //! algebra; restoring by copy is lossless for multi-part paths
    //! (`catalog.schema.table.column`) and quoted identifiers, which splitting a
    //! dotted string on '.' would not be.
    //!
    //! DECIDE variables are deliberately NOT routed through here: they are
    //! canonicalized to their unqualified name, which `bind_select_node.cpp`
    //! always registers (the qualified form is only an alias), so `keepS` and
    //! `S.keepS` stay one symbol and `decide_variables.count(name)` keeps working.
    unordered_map<string, unique_ptr<ParsedExpression>> column_map;

    //! Map of placeholder names to original MIN(...)/MAX(...) aggregate calls
    //! (opaque through normalization, like abs_map). MIN/MAX used to be
    //! represented as `__MIN__ * inner` / `__MAX__ * inner` so the symbol
    //! algebra could see (and reassemble) the inner expression, but a
    //! multi-term inner (`MIN((qty+1)*x)`) gets auto-distributed by
    //! `.expand()` before the marker can be reassociated with the whole
    //! product, scrambling the aggregate. Keeping the whole MIN/MAX node
    //! opaque — same idiom as abs_map/subquery_map — sidesteps that: the CAS
    //! never sees inside it, so there is nothing for it to distribute.
    unordered_map<string, unique_ptr<ParsedExpression>> min_max_map;

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
unique_ptr<ParsedExpression> SimplifyDecideConstraints(const ParsedExpression &expr,
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
unique_ptr<ParsedExpression> SimplifyDecideObjective(const ParsedExpression &expr,
                                                      const case_insensitive_map_t<idx_t> &decide_variables,
                                                      double &out_constant_offset);

//! Produce a Graphviz DOT representation of a ParsedExpression tree
string ExpressionToDot(const ParsedExpression &expr);

} // namespace duckdb

