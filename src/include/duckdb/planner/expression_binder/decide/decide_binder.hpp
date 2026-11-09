//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/decide/decide_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/decide/decide_parsed_markers.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception.hpp" // Required for NotImplementedException
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

class BindContext;
class FunctionExpression;
struct DecideSourceNames;

//! The names a key element may not take: every declared decision, as the user may spell it.
case_insensitive_set_t DecisionNames(const case_insensitive_map_t<idx_t> &variables);

//! Find or create the key scope of `per K: ...`, given the parser's `SCOPE_PER_TAG` marker
//! (children: the body, then the elements of K). Each element resolves against the FROM
//! clause alone, never an outer query: a declared decision is an error; otherwise it is a
//! column, else a relation standing for its stored columns. Equal keys -- order and
//! repetition aside -- share one key (BindKey) and one scope. `key_text` is the key as
//! written, kept for display. A reducer's `per ()` is the empty key: one scope every row
//! belongs to.
idx_t FindOrCreateKeyScope(ClientContext &context, BindContext &bind_context, const FunctionExpression &per,
                           const case_insensitive_set_t &decision_names, const string &key_text,
                           vector<DecideKey> &keys, vector<KeyedScopeInfo> &keyed_scopes,
                           vector<idx_t> *written_order = nullptr);

//! The entry of `keys` for the columns of `per K` (the parser's `SCOPE_PER_TAG` marker),
//! created when no key so far has them: a column element compares NULL as SQL's `=`
//! does, a relation element stands for its stored columns, which compare NULL as a
//! value. `key_text` is the key as written, kept for display.
idx_t BindKey(ClientContext &context, BindContext &bind_context, const FunctionExpression &per,
              const case_insensitive_set_t &decision_names, const string &key_text, vector<DecideKey> &keys,
              vector<idx_t> *written_order = nullptr);

//! What a reducer's own key (`sum(per K: e)`, `SUM(D: e)`) and `by` need in order to
//! resolve. Owned by the declarations binder, which is still filling `keyed_scopes` when
//! the constraint and objective binders run; the binders hold a pointer so a reducer can
//! add a key-only scope to the same list.
struct DecideQualifierContext {
    //! Table index the decision-variable column refs bind to.
    idx_t decide_index = DConstants::INVALID_INDEX;
    vector<KeyedScopeInfo> *keyed_scopes = nullptr;
    //! Every key bound so far; a reducer's GROUP_KEY_TAG_PREFIX tag names its `by` key.
    vector<DecideKey> *keys = nullptr;
    //! The text the user wrote for the DECIDE tail (SelectNode::decide_text), which a node's query location indexes.
    const DecideSourceText *source_text = nullptr;

    //! The text written for the scope or `by` marker `marker`, as the user wrote it; false when the parser kept none.
    bool WrittenText(const ParsedExpression &marker, DecideSpanRole role, string &text) const {
        return source_text && source_text->Cut(marker.GetQueryLocation(), role, text);
    }
};

bool IsVariableExpression(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

//! `expr` as a message quotes it: the span of the query the user wrote for it (`text`, which its query location
//! indexes), else, for a node the parser kept no span for, its SQL without the tags the binders put on its nodes.
string DecideWrittenSql(const ParsedExpression &expr, const DecideSourceText *text);

bool ValidateSumArgument(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                         const DecideSourceText *text, string &error_msg);

bool ExpressionContainsDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

//! A function's name as the user wrote it: `count(*)` reaches the binder as `count_star`.
string DecideWrittenFunctionName(const FunctionExpression &func);

//! Reject a user-written CAST/TRY_CAST/:: whose child contains a DECIDE variable.
//!
//! This must run on the parsed tree before any DECIDE rewrite or binding. After
//! binding, an explicit cast and a cast DuckDB inserted to reconcile types are both
//! BoundCastExpression and authorship can no longer be recovered. Parser-internal
//! casts have no query location and are deliberately ignored here.
void ValidateDecideNoExplicitDecisionCasts(const ParsedExpression &expr,
                                           const case_insensitive_map_t<idx_t> &variables,
                                           const DecideSourceText *text);

//! Where `when`, `per` and `by` may stand, checked on the parsed constraints (`objective`
//! false) or objective before binding. `[when c] [per K]:` stands in front of a clause or
//! of a SUM/AVG/MIN/MAX argument, `by (key)` right after a SUM/AVG/MIN/MAX, `norm` or
//! `count`; a condition, a key and a `by` key take data only, and a reducer may not hold a
//! reducer (the objective's two-level form aside). Anything else -- a scope inside a
//! condition, a key or an ordinary subquery -- answers "not supported yet".
void ValidateDecideScopeSpellings(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                  bool objective, const DecideSourceText *text);

//! Whether an objective (its `[when c] [per ()]:` peeled) has the two-level form
//! `outer(per k: inner(e) by (k'))`: an outer reducer, optionally `by ()`, over a `per`
//! whose body is one inner reducer with a `by`. Whether k and k' are the same key is
//! the binder's question.
bool IsTwoLevelObjective(const ParsedExpression &body);

bool ContainsDecideAggregate(const ParsedExpression &expr);

//! Reject an aggregate over table columns that multiplies or divides a decision (`SUM(avg(p) * x)`): it spans all
//! rows, so it is no per-row coefficient, and symbolic normalization would distribute the decision into it. Runs on
//! the parsed tree, before that normalization. Every other algebra refusal is DecideShapeCheck's.
void ValidateDecideNoDataAggregateFactor(ClientContext &context, const ParsedExpression &expr,
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
//! `variable_types` is indexed as the DECIDE columns are; `variables` maps each
//! declared name onto that index.
void ValidateDecideNoIntegerStepComparisonOnReal(const ParsedExpression &expr,
                                                 const case_insensitive_map_t<idx_t> &variables,
                                                 const vector<LogicalType> &variable_types,
                                                 const DecideSourceText *text);

//! Reject a `<`, `>` or `<>` whose compared side is not provably whole-numbered from
//! its **declared types**, naming the column and its type.
//!
//! The companion to the validator above, and the same refusal: that one reads the
//! decision's declared type off the DECIDE clause, this one reads every other operand's
//! type off the bound tree. Together they are the whole of the integrality gate — the
//! model builder refuses nothing a user can cause.
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
//! `names` quotes an operand of a clause as written.
void ValidateDecideIntegralComparisonOperands(const Expression &expr, idx_t decide_index,
                                              const DecideSourceNames &names);

//! Whether every value `expr` can take is whole, by declared types, every addend counted:
//! `sum(x + 0.5)` is not, `sum(power(x, 2))` is. The test a helper decision standing for
//! a reducer takes (stage 05's value rewrite), by the same rules as the gate above.
bool DecideValueIsWholeNumbered(const Expression &expr, idx_t decide_index);

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
    //! refusal raised on it — the integrality gate and the degree gate — printed
    //! without the `LINE`/caret that makes a long SUCH THAT clause navigable.
    //! Applied in each `BindExpression` override, which is where the recursion enters
    //! a node, so inner nodes keep their own location rather than the root's.
    //!
    //! Takes the location by value rather than the parsed node: binding the `norm`
    //! marker replaces `expr_ptr` in place, so by the time the result comes back the
    //! node the caller held is no longer the one the user wrote.
    static BindResult PreserveQueryLocation(optional_idx location, BindResult result);
    //! Throws a clause's error at its root unless a column or table was not found. DuckDB
    //! retries a failed expression in each outer query, in case it refers to one, and reports
    //! that retry's error; only a missing name can be such a reference, so any other error is
    //! the clause's own verdict and must reach the user as it is.
    static BindResult ThrowClauseError(BindResult result, bool root_expression);

    //! True when `expr`, a reference to a decision (IsVariableExpression), names a query-wide (`per ()`) one.
    //! A scalar has one solver column, so it needs no reducer to collapse it —
    //! and conversely may not appear inside one on its own.
    bool IsScalarDecideVariable(const ParsedExpression &expr) const;
    //! True when `expr` holds the same value on every input row: every leaf is
    //! either a literal/constant, a subquery that names no per-row decision, or a query-wide
    //! (`per ()`) decision — no data column and no per-row or keyed decision
    //! appears anywhere inside it. A reducer around a row-invariant body has
    //! nothing to reduce over (SUM(cap), SUM(cap1 + cap2)); a reducer whose body
    //! mixes a scalar with row-varying data (SUM(cost * cap)) is not row-invariant
    //! and is legal — the scalar just contributes uniformly to every term.
    bool IsRowInvariantExpression(const ParsedExpression &expr) const;
    //! True when `expr`, or the body of a subquery in it, names a decision that is not query-wide.
    bool ReadsRowDecision(const ParsedExpression &expr) const;
    //! True when `expr`, sitting below a clause's top, is plain data that DuckDB binds by its
    //! own rules: it reads no decision, and it is not a function (a reducer or a scope
    //! marker) or a subquery, which keep their DECIDE handling. A CASE over data is plain data.
    bool IsDataOperand(const ParsedExpression &expr) const;

    BindResult BindAggregate(FunctionExpression &aggr, AggregateFunctionCatalogEntry &func, idx_t depth) override;
    //! Binds the colon form `agg(D, T: expr)` as `agg(per D, T: expr)` (language/guide.md §5.1): the
    //! relations' stored columns as an exact key scope and the tag of that scope.
    BindResult BindQualifiedReducer(FunctionExpression &qualified_expr, idx_t depth);
    //! Binds `agg([when c] [per K]: e)`: `agg(e)` with `c` as the aggregate's filter and
    //! `K` as the key it keeps one row of each class by (an exact key scope, tagged like a
    //! qualifier; `per ()` is the empty key).
    BindResult BindScopedReducer(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
    //! Binds `agg(...) BY (key)`: the reducer, then the key into LogicalDecide::keys,
    //! named on the aggregate by a GROUP_KEY_TAG_PREFIX tag (none for `BY ()`).
    BindResult BindReducerBy(FunctionExpression &by, idx_t depth);
    BindResult BindFunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
    BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                              bool root_expression = false) override;
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

    //! Binds a data expression (a `when` condition, a `by` key element) through the base ExpressionBinder, which
    //! bypasses DECIDE-specific dispatch since neither may reference a DECIDE variable. The error, if any, is
    //! returned.
    ErrorData BindDataChild(unique_ptr<ParsedExpression> &expr, idx_t depth);

    bool is_top_expression;
    //! Set while BindDataChild binds.
    bool binding_when_condition = false;
    case_insensitive_map_t<idx_t> variables;
    //! Subset of `variables` that are query-wide (`per (): x`).
    case_insensitive_set_t scalar_variables;
    //! Null when the caller cannot resolve qualifiers (relation-qualified
    //! reducers are then rejected rather than silently ignored).
    optional_ptr<DecideQualifierContext> qualifier_context;
};

} // namespace duckdb
