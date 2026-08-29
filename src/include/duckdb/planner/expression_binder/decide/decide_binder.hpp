//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/decide/decide_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception.hpp" // Required for NotImplementedException
#include "duckdb/planner/operator/logical_decide.hpp"

namespace duckdb {

class BindContext;
class FunctionExpression;

//! Find or create the entity scope keyed by `table_name`, returning its index into
//! `entity_scopes`. Shared by the declaration path (`T.x(INT)`) and the
//! relation-qualified reducer path (`sum(T: ...)`): a qualifier is an entity scope
//! that may carry no variable, so both build the same tuple-identity key the same
//! way and both feed the one `EntityMapping` the executor builds per scope.
idx_t FindOrCreateEntityScope(BindContext &bind_context, const string &table_name,
                              vector<EntityScopeInfo> &entity_scopes,
                              case_insensitive_map_t<idx_t> &table_scope_map);

//! Composite form for a reducer qualified by several relations at once
//! (`sum(D, T: ...)`). The scope's tuple identity is the concatenation of each
//! named relation's own key; de-duplication then collapses fan-out contributed
//! only by relations *not* named. `table_names` is canonicalized (sorted,
//! case-insensitively) before being used as the cache key, so `sum(D,T: ...)`
//! and `sum(T,D: ...)` share one scope.
idx_t FindOrCreateEntityScope(BindContext &bind_context, const vector<string> &table_names,
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

bool IsVariableExpression(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

bool ValidateSumArgument(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables, string &error_msg,
                         bool allow_quadratic = false);

bool ExpressionContainsDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

//! The one wording for a `CASE` written anywhere in a DECIDE clause.
//!
//! `CASE` is rejected in three places -- inside a reducer argument, and as an
//! unsupported expression class in each of the constraint and objective binders --
//! and a user who writes one is asking for conditional logic whichever spelling
//! they used. Sharing the text keeps the answer (postfix `WHEN`, `PER`, or a CTE)
//! in front of all of them instead of leaking `ExpressionClass::CASE`.
const char *DecideCaseUnsupportedMessage();

//! Reject a user-written CAST/TRY_CAST/:: whose child contains a DECIDE variable.
//!
//! This must run on the parsed tree before any DECIDE rewrite or binding. After
//! binding, an explicit cast and a cast DuckDB inserted to reconcile types are both
//! BoundCastExpression and authorship can no longer be recovered. Parser-internal
//! casts have no query location and are deliberately ignored here.
void ValidateDecideNoExplicitDecisionCasts(const ParsedExpression &expr,
                                           const case_insensitive_map_t<idx_t> &variables);

bool IsDecideAggregateName(const string &name);
bool ContainsDecideAggregate(const ParsedExpression &expr);
bool ContainsWhenOperator(const ParsedExpression &expr);

//! Reject non-linear scalar functions (SQRT, EXP, LOG, FLOOR, ...) that wrap a
//! DECIDE variable. Throws BinderException on violation. Runs on the parsed tree,
//! catching cases that would otherwise silently strip the scalar in per-row
//! constraints or reach the model builder as an unreadable term. Uses the
//! catalog to distinguish scalar from aggregate functions so aggregate-shaped
//! mis-uses (e.g., BIT_AND(x), STDDEV(x)) fall through to BindAggregate's
//! aggregate-specific error instead.
void ValidateDecideNoNonLinearScalar(ClientContext &context,
                                     const ParsedExpression &expr,
                                     const case_insensitive_map_t<idx_t> &variables);

//! Reject a `<`, `>` or `<>` whose comparison references a REAL decision variable.
//!
//! All three are encoded by stepping the bound one integer unit: `< K` becomes
//! `<= K-1`, and `<> K` becomes the disjunction `<= K-1 OR >= K+1`. Both are exact
//! only when the compared side is confined to integer points. A REAL decision has no
//! such confinement, so `<` would cut feasible continuous solutions and return a
//! wrong optimum, and `<>` would excise a whole band around a point that has no
//! width. (`{v : v != K}` over the reals is an open set, and no MILP feasible region
//! — a finite union of closed polyhedra — can represent one, so `<>` on a continuous
//! quantity has no correct encoding at all, not merely an inexact one.)
//!
//! This is the structural half of a refusal that also has a value half: a data
//! column can produce a fractional coefficient over integer decisions, which is
//! knowable only after the scan and stays in the model builder. The declared type
//! is knowable here, from the query text alone, so this half rejects at bind time
//! and can name the variable and the clause the user wrote.
//!
//! The rule is stated on the type of the *compared quantity*, not on every type
//! appearing beneath it: `norm(e, 0, M)` counts nonzeros and is integer-valued by
//! definition, so a REAL `e` underneath one is not what is being compared and does
//! not trigger the refusal. See `IsIntegerValuedReducer`.
//!
//! `variable_types` is indexed as the DECIDE columns are; `variables` maps every
//! spelling (bare and table-qualified) of a declared name onto that index.
void ValidateDecideNoIntegerStepComparisonOnReal(const ParsedExpression &expr,
                                                 const case_insensitive_map_t<idx_t> &variables,
                                                 const vector<LogicalType> &variable_types);

//! Reject a `<`, `>` or `<>` whose compared side is not provably whole-numbered from
//! its **declared types**, naming the column and its type.
//!
//! The companion to the validator above, and the same refusal: that one reads the
//! decision's declared type off the DECIDE clause, this one reads every other operand's
//! type off the bound tree. Together they are the whole of the integrality gate — the
//! model builder no longer refuses anything a user can cause.
//!
//! **This is a type judgement, not a value judgement.** A `DECIMAL(15,2)` column whose
//! rows all happen to hold whole numbers is still refused, because the alternative makes
//! a query's *validity* depend on which rows are in the table: inserting one fractional
//! row would make a working query illegal. The declared type is the contract, and the
//! message names the cast that states the assumption explicitly. `DECIMAL(p, 0)` carries
//! its scale in the type and so is accepted without one.
//!
//! Two shapes are whole-numbered whatever they contain, and are exempt:
//!   - `norm(e, 0, M)` counts nonzeros, so it is a count however `e` is typed.
//!   - `AVG(e) <> K` only, whose denominator is hoisted to the right-hand side
//!     (`SUM(e) <> K*n`), leaving an integral left side. The hoist is specific to `<>`;
//!     `AVG(e) < K` keeps its fractional 1/n coefficients and is refused.
//!
//! Runs on the bound tree, because a parsed tree has no types yet.
//! `decide_index` identifies the DECIDE binding, so a side that references no decision
//! can be recognised as the bound `K` and skipped.
void ValidateDecideIntegralComparisonOperands(const Expression &expr, idx_t decide_index);

//! The DecideBinder is a base class for binders in DECIDE statements
class DecideBinder : public ExpressionBinder {
public:
    DecideBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables,
                 const case_insensitive_set_t &scalar_variables = case_insensitive_set_t(),
                 optional_ptr<DecideQualifierContext> qualifier_context = nullptr);

protected:
    //! Carry the parsed node's source location onto the bound node it produced.
    //!
    //! `ExpressionBinder::Bind` does this at its own entry point, but the DECIDE
    //! binders dispatch through `BindExpression`, which bypasses it. The bound tree
    //! therefore reached the post-binding validators with no location, and every
    //! refusal raised on it — the integrality gate, and now the degree gate — printed
    //! without the `LINE`/caret that makes a long SUCH THAT clause navigable.
    //! Applied in each `BindExpression` override, which is where the recursion enters
    //! a node, so inner nodes keep their own location rather than the root's.
    //!
    //! Takes the location by value rather than the parsed node: binding the `norm`
    //! marker replaces `expr_ptr` in place, so by the time the result comes back the
    //! node the caller held is no longer the one the user wrote.
    static BindResult PreserveQueryLocation(optional_idx location, BindResult result);

    //! True when `expr` is a bare reference to a query-wide (`scalar`) decision.
    //! A scalar has one solver column, so it needs no reducer to collapse it —
    //! and conversely may not appear inside one on its own.
    bool IsScalarDecideVariable(const ParsedExpression &expr) const;
    //! True when `expr` holds the same value on every input row: every leaf is
    //! either a literal/constant, an uncorrelated subquery, or a query-wide
    //! (`scalar`) decision — no data column and no row- or entity-scoped decision
    //! appears anywhere inside it. A reducer around a row-invariant body has
    //! nothing to reduce over (SUM(cap), SUM(cap1 + cap2)); a reducer whose body
    //! mixes a scalar with row-varying data (SUM(cost * cap)) is not row-invariant
    //! and is legal — the scalar just contributes uniformly to every term.
    bool IsRowInvariantExpression(const ParsedExpression &expr) const;

    BindResult BindAggregate(FunctionExpression &aggr, AggregateFunctionCatalogEntry &func, idx_t depth) override;
    BindResult BindLocalWhenAggregate(FunctionExpression &when_expr, idx_t depth);
    //! Binds `agg(D: expr)`: resolves `D` to an entity scope, enforces the §3.2.2
    //! well-formedness rule on the reducer body, and tags the bound aggregate with
    //! the scope it de-duplicates by.
    BindResult BindQualifiedReducer(FunctionExpression &qualified_expr, idx_t depth);
    BindResult BindFunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
    BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression = false) override;
    //! The dispatch itself. `BindExpression` wraps it so every bound node this binder
    //! produces carries a source location, whatever path the dispatch takes.
    BindResult BindExpressionInternal(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression);
    virtual DecideExpression GetExpressionType(ParsedExpression &expr, string &error_msg) {
        throw duckdb::NotImplementedException("GetExpressionType is not implemented for this binder.");
    }

    //! Shared `norm`/SUM/AVG/MIN/MAX reducer classification between
    //! `DecideConstraintsBinder` and `DecideObjectiveBinder`. Returns true when `func`
    //! is one of these reducer names, with `result`/`error_msg` holding the verdict;
    //! returns false for any other function name so the caller falls through to its
    //! own remaining classification (the two binders diverge there — a SUCH THAT
    //! left-hand side and a MAXIMIZE/MINIMIZE objective accept different shapes).
    bool ClassifyReducerCall(FunctionExpression &func, DecideExpression &result, string &error_msg);

    //! Shared PER-wrapper assembly between `DecideConstraintsBinder` and
    //! `DecideObjectiveBinder`: `func.children[0]` is the inner constraint/objective
    //! (possibly WHEN-wrapped), bound through the subclass's own dispatch;
    //! `func.children[1..]` are the PER grouping columns, bound through the base
    //! `ExpressionBinder` since a PER column is a plain table column, not a DECIDE
    //! expression. Assembles both into one `BoundConjunctionExpression` tagged with
    //! `func.function_name`, which `DecideCanonicalizer` reads back as the PER tag.
    BindResult BindPerWrapper(FunctionExpression &func, idx_t depth);

    bool is_top_expression;
    //! Set while binding a WHEN condition or a PER column: both bypass DECIDE-specific
    //! dispatch and bind through the base ExpressionBinder instead, since neither may
    //! reference a DECIDE variable.
    bool binding_when_condition = false;
    case_insensitive_map_t<idx_t> variables;
    //! Subset of `variables` declared with the `scalar` keyword.
    case_insensitive_set_t scalar_variables;
    //! Null when the caller cannot resolve qualifiers (relation-qualified
    //! reducers are then rejected rather than silently ignored).
    optional_ptr<DecideQualifierContext> qualifier_context;
};

} // namespace duckdb
