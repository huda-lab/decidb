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

class Binder;
class BindContext;
class ColumnRefExpression;
class DecideFrameExpression;
class FunctionExpression;
struct DecideSourceNames;

//! The names a key element may not take: every declared decision, as the user may spell it.
case_insensitive_set_t DecisionNames(const case_insensitive_map_t<idx_t> &variables);

//! The texts several sites raise, one builder each (language/errors.md). Each states what is wrong and stops: the
//! caret names the place, and `q` is a fragment cut from the query.
//!
//! The refusal of a DECIDE that reads a column of an outer query (a LATERAL subquery's or a scalar
//! subquery's outer query): it solves one problem, so it cannot be re-solved per outer row. One message
//! for every way the column is reached.
string OuterQueryColumnMessage(const string &column_name);
//! A decision read where only data may stand: `where` is `when, per and by`, `WHERE`, or `a frame's order key and
//! within`.
string DecisionInDataMessage(const string &decision, const string &where);
//! A comparison, BETWEEN or conjunction that reads a decision, used as a number: `x <= (x > 1)`.
string ComparisonUsedAsNumberMessage(const string &expression);
//! A call given the wrong number of arguments.
string ArgumentCountMessage(const string &call, idx_t count);
//! A subquery in a clause that is not scalar, or that reads a decision.
string SubqueryNotScalarMessage();
string SubqueryReadsDecisionMessage();
//! Throws that refusal, naming `column` as written, when `column`, which did not resolve in `binder`'s own
//! query, resolves in an outer one. The outer binders are asked only to qualify the name, as DuckDB's retry does, and
//! bind nothing. A DECIDE clause is rebound in the outer binder by that retry, which knows no DECIDE construct, so the
//! refusal is raised here, at the column, before any retry. A name that resolves nowhere is left to its
//! caller's own "not found".
void ThrowIfOuterColumn(Binder &binder, const ColumnRefExpression &column);

//! Find or create the key scope of `per K: ...`, given the elements of K as written. Each element resolves against
//! the FROM clause alone, never an outer query: a declared decision is an error; otherwise it is a
//! column, else a relation standing for its stored columns. Equal keys -- order and
//! repetition aside -- share one key (BindKey) and one scope. `key_text` is the key as
//! written, kept for display. A reducer's `per ()` is the empty key: one scope every row
//! belongs to.
idx_t FindOrCreateKeyScope(ClientContext &context, Binder &binder, const DecideKeyView &key,
                           const case_insensitive_set_t &decision_names, const string &key_text,
                           const DecideSourceText *text, vector<DecideKey> &keys, vector<KeyedScopeInfo> &keyed_scopes);

//! The entry of `keys` for the columns of `per K`, given the elements of K as written,
//! created when no key so far has them: a column element compares NULL as SQL's `=`
//! does, a relation element stands for its stored columns, which compare NULL as a
//! value. `key_text` is the key as written, kept for display; each column element is named as `text` has it written
//! (null: as it prints), the way a message quotes it.
idx_t BindKey(ClientContext &context, Binder &binder, const DecideKeyView &key,
              const case_insensitive_set_t &decision_names, const string &key_text, const DecideSourceText *text,
              vector<DecideKey> &keys);

//! What a reducer's own key (`sum(per K: e)`) and `by` need in order to
//! resolve. Owned by the declarations binder, which is still filling `keyed_scopes` when
//! the constraint and objective binders run; the binders hold a pointer so a reducer can
//! add a key-only scope to the same list.
struct DecideQualifierContext {
	//! Table index the decision-variable column refs bind to.
	idx_t decide_index = DConstants::INVALID_INDEX;
	vector<KeyedScopeInfo> *keyed_scopes = nullptr;
	//! Every key bound so far; a reducer's GROUP_KEY_TAG_PREFIX tag names its `by` key.
	vector<DecideKey> *keys = nullptr;
	//! Every frame selector bound so far; a frame's FRAME_TAG_PREFIX tag indexes it.
	vector<DecideFrameSpec> *frames = nullptr;
	//! The text the user wrote for the DECIDE tail (SelectNode::decide_text), which a node's query location indexes.
	const DecideSourceText *source_text = nullptr;

	//! The text written for the scope or `by` marker `marker`, as the user wrote it; false when the parser kept none.
	bool WrittenText(const ParsedExpression &marker, DecideSpanRole role, string &text) const {
		return source_text && source_text->Cut(marker.GetQueryLocation(), role, text);
	}
};

bool IsVariableExpression(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

//! Refuses `name`, a decision's or a column a clause reads, at `at` when it contains a DECIDE tag (R20).
void RefuseDecideTagInName(const ParsedExpression &at, const string &name);

//! `expr` as a message quotes it: the span of the query the user wrote for it (`text`, which its query location
//! indexes), else, for a node the parser kept no span for, its SQL without the tags the binders put on its nodes.
string DecideWrittenSql(const ParsedExpression &expr, const DecideSourceText *text);

bool ExpressionContainsDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

//! Reject a user-written CAST/TRY_CAST/:: whose child contains a DECIDE variable.
//!
//! This must run on the parsed tree before any DECIDE rewrite or binding. After
//! binding, an explicit cast and a cast DuckDB inserted to reconcile types are both
//! BoundCastExpression and authorship can no longer be recovered. Parser-internal
//! casts have no query location and are deliberately ignored here.
void ValidateDecideNoExplicitDecisionCasts(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                           const DecideSourceText *text);

//! Where `when`, `per` and `by` may stand, checked on one parsed clause (a constraint or the
//! objective) before binding. `[when c] [per K]:` stands in front of a clause or
//! of a SUM/AVG/MIN/MAX argument, `by (key)` right after a SUM/AVG/MIN/MAX, `norm` or
//! `count`; a condition, a key and a `by` key take data only. A reducer may hold reducers
//! to any depth (§5.5), but not a norm, and one over decisions not one over data alone
//! (D80). Anything else -- a scope inside a condition, a key or an ordinary subquery --
//! is refused.
void ValidateDecideScopeSpellings(const ParsedExpression &clause, const case_insensitive_map_t<idx_t> &variables,
                                  const DecideSourceText *text);

bool ContainsDecideAggregate(const ParsedExpression &expr);

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
	//! Throws a clause's error at its root. DuckDB would retry the failed expression in each outer
	//! query, in case it refers to one, and report that retry's error, which knows no DECIDE
	//! construct: a missing name inside a `when` would reach the user as an unknown function. A
	//! clause reads no outer query (ThrowIfOuterColumn refuses one where its column fails to
	//! resolve), so the clause's own error is the verdict.
	static BindResult ThrowClauseError(BindResult result, bool root_expression);

	//! A column of a clause that its own query does not hold is refused when an outer query does
	//! (ThrowIfOuterColumn); any other missing name keeps DuckDB's error.
	unique_ptr<ParsedExpression> QualifyColumnName(ColumnRefExpression &col_ref, ErrorData &error) override;
	using ExpressionBinder::QualifyColumnName;

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
	//! Binds `agg([when c] [per K]: e)`: `agg(e)` with `c` as the aggregate's filter and
	//! `K` as the key it keeps one row of each class by (an exact key scope, tagged like a
	//! qualifier; `per ()` is the empty key).
	BindResult BindScopedReducer(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
	//! Binds `agg(...) BY (key)`: the reducer, then the key into LogicalDecide::keys,
	//! named on the aggregate by a GROUP_KEY_TAG_PREFIX tag (none for `BY ()`).
	BindResult BindReducerBy(FunctionExpression &by, idx_t depth);
	//! Binds a frame selector (language/guide.md §5.6) to an aggregate over its payload
	//! -- `first(e)` for a point frame, its reducer for a range -- tagged by
	//! FRAME_TAG_PREFIX with the frame's entry in LogicalDecide::frames, which holds the
	//! order key, the `within` key's columns, the positions and the fallback.
	BindResult BindDecideFrame(DecideFrameExpression &frame, idx_t depth);
	//! A frame's `else`, bound into `out`: a number, and none when it is NULL before the
	//! query runs (D86).
	ErrorData BindFrameFallback(DecideFrameExpression &frame, const string &fallback_text, idx_t depth,
	                            unique_ptr<Expression> &out);
	//! The catalog aggregate `name` (a range's reducer, or a point's `first`) over a bound payload, aliased `alias`;
	//! null, with `error` set, when none takes its type.
	unique_ptr<Expression> BindFrameAggregate(const string &name, unique_ptr<Expression> payload,
	                                          const DecideFrameExpression &frame, const string &alias,
	                                          ErrorData &error);
	BindResult BindFunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
	BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
	                          bool root_expression = false) override;
	//! The dispatch itself. `BindExpression` wraps it so every bound node this binder
	//! produces carries a source location, whatever path the dispatch takes.
	BindResult BindExpressionInternal(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression);
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
