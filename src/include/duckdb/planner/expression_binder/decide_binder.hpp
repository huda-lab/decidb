//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/decide_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception.hpp" // Required for NotImplementedException
#include "duckdb/decidb/utility/debug.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"

namespace duckdb {

class BindContext;

//! Find or create the entity scope keyed by `table_name`, returning its index into
//! `entity_scopes`. Shared by the declaration path (`T.x(INT)`) and the
//! relation-qualified reducer path (`sum(T: ...)`): a qualifier is an entity scope
//! that may carry no variable, so both build the same tuple-identity key the same
//! way and both feed the one `EntityMapping` the executor builds per scope.
idx_t FindOrCreateEntityScope(BindContext &bind_context, const string &table_name,
                              vector<EntityScopeInfo> &entity_scopes,
                              case_insensitive_map_t<idx_t> &table_scope_map);

//! Peels the parser's qualified-reducer wrapper so checks that key off the
//! aggregate's name see the aggregate: `sum(D: e)` reads as `sum(e)`. Returns
//! `expr` unchanged when it is not a qualified reducer.
const ParsedExpression &UnwrapQualifiedReducer(const ParsedExpression &expr);

//! What a relation-qualified reducer needs in order to resolve `D:` and to check
//! the §3.2.2 well-formedness rule. Owned by bind_select_node, which is still
//! filling `entity_scopes` when the constraint and objective binders run; the
//! binders hold a pointer so a qualifier can add a key-only scope to the same list.
struct DecideQualifierContext {
    //! Table index the decision-variable column refs bind to.
    idx_t decide_index = DConstants::INVALID_INDEX;
    vector<EntityScopeInfo> *entity_scopes = nullptr;
    case_insensitive_map_t<idx_t> *table_scope_map = nullptr;
    //! Scope of each declared variable, indexed as the decide columns are.
    const vector<DecideVarScopeInfo> *variable_scopes = nullptr;
};

bool IsScalarValue(ParsedExpression &expr);

bool IsVariableExpression(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

bool ValidateSumArgument(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables, string &error_msg,
                         bool allow_quadratic = false, bool allow_bilinear = false);

bool ContainsQuadraticPattern(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

bool ExpressionContainsDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

bool IsDecideAggregateName(const string &name);
bool ContainsDecideAggregate(const ParsedExpression &expr);
bool ContainsWhenOperator(const ParsedExpression &expr);

//! Reject non-linear scalar functions (SQRT, EXP, LOG, FLOOR, ...) that wrap a
//! DECIDE variable. Throws BinderException on violation. Safe to call before
//! symbolic normalization — catches cases that would otherwise silently strip
//! the scalar in per-row constraints or crash in the symbolic layer. Uses the
//! catalog to distinguish scalar from aggregate functions so aggregate-shaped
//! mis-uses (e.g., BIT_AND(x), STDDEV(x)) fall through to BindAggregate's
//! aggregate-specific error instead.
void ValidateDecideNoNonLinearScalar(ClientContext &context,
                                     const ParsedExpression &expr,
                                     const case_insensitive_map_t<idx_t> &variables);


// inline void DebugPrintParsed(const string &tag, const ParsedExpression &expr) {
// 	deb("[BINDER] ", tag, ": ", expr.ToString());
// }
// inline void DebugPrintBound(const string &tag, const Expression &expr) {
// 	deb("[BINDER] ", tag, ": ", expr.ToString());
// }

//! The DecideBinder is a base class for binders in DECIDE statements
class DecideBinder : public ExpressionBinder {
public:
    DecideBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables,
                 const case_insensitive_set_t &scalar_variables = case_insensitive_set_t(),
                 optional_ptr<DecideQualifierContext> qualifier_context = nullptr);

protected:
    //! True when `expr` is a bare reference to a query-wide (`scalar`) decision.
    //! A scalar has one solver column, so it needs no reducer to collapse it —
    //! and conversely may not appear inside one.
    bool IsScalarDecideVariable(const ParsedExpression &expr) const;
    //! Name of the query-wide decision referenced anywhere inside `expr`, or ""
    //! if there is none. Used to reject scalars inside reducers.
    string FindScalarDecideVariable(const ParsedExpression &expr) const;

    BindResult BindAggregate(FunctionExpression &aggr, AggregateFunctionCatalogEntry &func, idx_t depth) override;
    BindResult BindLocalWhenAggregate(FunctionExpression &when_expr, idx_t depth);
    //! Binds `agg(D: expr)`: resolves `D` to an entity scope, enforces the §3.2.2
    //! well-formedness rule on the reducer body, and tags the bound aggregate with
    //! the scope it de-duplicates by.
    BindResult BindQualifiedReducer(FunctionExpression &qualified_expr, idx_t depth);
    BindResult BindFunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
    BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression = false) override;
    virtual DecideExpression GetExpressionType(ParsedExpression &expr, string &error_msg) {
        throw duckdb::NotImplementedException("GetExpressionType is not implemented for this binder.");
    }

    bool is_top_expression;
    case_insensitive_map_t<idx_t> variables;
    //! Subset of `variables` declared with the `scalar` keyword.
    case_insensitive_set_t scalar_variables;
    //! Null when the caller cannot resolve qualifiers (relation-qualified
    //! reducers are then rejected rather than silently ignored).
    optional_ptr<DecideQualifierContext> qualifier_context;
};

} // namespace duckdb
