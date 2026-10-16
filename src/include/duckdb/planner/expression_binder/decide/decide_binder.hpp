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
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

class BindContext;
class FunctionExpression;
struct DecideSourceNames;

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

//! Find or create the key scope of `per K: ...`, given the parser's `SCOPE_PER_TAG` marker
//! (children: the body, then the elements of K). Each element resolves against the FROM
//! clause alone, never an outer query: a declared decision is an error; otherwise it is a
//! column, else a relation standing for its stored columns. Equal keys -- order and
//! repetition aside -- share one scope, marked `exact_key` so it is never refined and never
//! shared with a `T.x` scope. `key_text` is the key as written, kept for display. A
//! reducer's `per ()` is the empty key: one scope every row belongs to.
idx_t FindOrCreateKeyScope(BindContext &bind_context, const FunctionExpression &per,
                           const case_insensitive_set_t &decision_names, const string &key_text,
                           vector<EntityScopeInfo> &entity_scopes);

//! The columns of `per K` (the parser's `SCOPE_PER_TAG` marker) as bound column
//! references, resolved and ordered as FindOrCreateKeyScope resolves them; none for
//! `per ()`. The bound form of a constraint's generation key.
vector<unique_ptr<Expression>> BindKeyColumns(BindContext &bind_context, const FunctionExpression &per,
                                              const case_insensitive_set_t &decision_names);

//! Peels the parser's `BY (key)` and qualified-reducer markers so checks that key off
//! the aggregate's name see the aggregate: `sum(D: e) by (g)` reads as `sum(e)`.
//! Returns `expr` unchanged when it carries neither.
const ParsedExpression &UnwrapReducerMarkers(const ParsedExpression &expr);

//! A reducer argument without its `[WHEN c] [PER K]:` scope: `sum(when c: e)` sums `e`.
const ParsedExpression &ReducerArgumentBody(const ParsedExpression &argument);

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
    //! Every `by (key)` bound so far; a reducer's GROUP_KEY_TAG_PREFIX tag indexes it.
    vector<DecideGroupKey> *group_keys = nullptr;
    //! The query's source fragments (TagDecideSourceFragments): a `per K:` in front of a
    //! constraint adds its key as written, which binding expands past recognition.
    vector<string> *source_fragments = nullptr;
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

//! Where the scope spellings may stand, checked on the parsed constraints (`objective`
//! false) or objective before binding. `[WHEN c] [PER K]:` stands in front of a clause or
//! of a SUM/AVG/MIN/MAX argument, `BY (key)` right after a SUM/AVG/MIN/MAX; a condition,
//! a key and a `by` key take data only. A clause is written in the old spellings
//! (postfix WHEN/PER, `SUM(e) WHEN c`) or the new ones, never both, and with a new
//! spelling a reducer may not hold a reducer (the objective's two-level form aside).
//! Anything else -- a spelling inside a condition, a key or an ordinary subquery --
//! answers "not supported yet".
void ValidateDecideScopeSpellings(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                  bool objective);

//! Whether `expr` is the parser's operator marker named `tag` (decide.hpp).
bool IsDecideMarker(const ParsedExpression &expr, const char *tag);

//! Whether a constraint or the objective is written with a scope spelling: a `when` or
//! `per` in front, a `by`, or a scope in front of a reducer's argument. Such a clause is
//! bound under the scoping language's rules (SCOPED_CLAUSE_TAG).
bool UsesScopeSpelling(const ParsedExpression &clause);

//! Whether an objective (its `[WHEN c] [PER ()]:` peeled) has the two-level form
//! `outer(PER k: inner(e) BY (k'))`: an outer reducer, optionally `BY ()`, over a `per`
//! whose body is one inner reducer with a `by`. Whether k and k' are the same key is
//! the binder's question.
bool IsTwoLevelObjective(const ParsedExpression &body);

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
//! `names` quotes an operand of a clause written with the scope spellings as written.
void ValidateDecideIntegralComparisonOperands(const Expression &expr, idx_t decide_index,
                                              optional_ptr<const DecideSourceNames> names = nullptr);

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
    //! Binds `agg([WHEN c] [PER K]: e)`: `agg(e)` with `c` as the aggregate's filter and
    //! `K` as the key it keeps one row of each class by (an exact key scope, tagged like a
    //! qualifier; `PER ()` is the empty key).
    BindResult BindScopedReducer(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
    //! Binds `agg(...) BY (key)`: the reducer, then the key into LogicalDecide::group_keys,
    //! named on the aggregate by a GROUP_KEY_TAG_PREFIX tag (none for `BY ()`).
    BindResult BindReducerBy(FunctionExpression &by, idx_t depth);
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
    //! Subset of `variables` that are query-wide (`scalar x` or `per (): x`).
    case_insensitive_set_t scalar_variables;
    //! Null when the caller cannot resolve qualifiers (relation-qualified
    //! reducers are then rejected rather than silently ignored).
    optional_ptr<DecideQualifierContext> qualifier_context;
};

} // namespace duckdb
