//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/optimizer/decide/decide_optimizer_internal.hpp
//
// Helpers shared between the DECIDE rewrite passes, which are split across
// decide_optimizer.cpp and its decide_rewrite_*.cpp siblings. Internal to
// layer 5 — nothing outside src/optimizer/decide/ includes this. The nested
// namespace keeps deliberately short names (StrToAggType, MakeTrueExpression,
// ScopeOf) out of `duckdb`; each rewrite file pulls it in with a
// file-scope `using namespace decide_rewrite;`.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <functional>

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

class ClientContext;
class DecideCanonicalizer;
class LogicalDecide;
class Optimizer;

namespace decide_rewrite {

//! Adds `check`, a message that raises the error of a value the model cannot read (VALUE_IS_VALID), to the DECIDE
//! node's scope checks, unless a check of the same value stands there. Two checks are of one value when they are equal
//! but for where each points (the place of a NaN or infinite value's error), so a value shared by two clauses is
//! checked once, and the place the first raises at stays (the first error reached is the one raised).
void AddValueCheck(LogicalDecide &decide, unique_ptr<Expression> check);

//! Map an aggregate's SQL name onto the objective aggregate enum.

//! Whether a lower-cased aggregate name is MIN or MAX.
inline bool IsExtremumName(const string &name) {
	return name == "min" || name == "max";
}

//! `SUM(body)` bound as a reducer, reading the rows `filter` keeps (null: every row).
unique_ptr<Expression> BindSumOver(Optimizer &optimizer, unique_ptr<Expression> body, const Expression *filter);

//! Recognize the binder's `norm` marker alias and return its payload.
bool TryParseNormMarker(const string &alias, string &payload);

//! Carry only the source clause's id onto `to`: the clause a message about it quotes, with none of the removal or
//! repair grouping a full copy brings.
void CopySourceClauseTag(const string &from_alias, Expression &to);

//! Carry a source clause's provenance tags onto an expression a rewrite emitted
//! in its place, so diagnostics still name the clause the user typed.
void CopyClauseProvenanceTags(const string &from_alias, Expression &to);

//! Stamp an expression as formulation-generated (a row this layer emitted rather
//! than one the user wrote), attributed to `source_alias`; a row defining auxiliary `defined` says so
//! (DEFINES_TAG_PREFIX).
void MarkFormulationConstraint(Expression &expr, const string &source_alias, idx_t defined = DConstants::INVALID_INDEX);

//! A TRUE placeholder standing in for a clause a rewrite has consumed.
unique_ptr<Expression> MakeTrueExpression(const string &source_alias);

//! `row`, which a rewrite adds for a clause, wrapped in a copy of the clause's `when` condition when it has one, so
//! it reads only the rows the clause keeps.
unique_ptr<Expression> ScopeHelperRow(unique_ptr<Expression> row, const Expression *when);

//! `row`, which a rewrite adds for a clause, in the clause's scope: its `when` (ScopeHelperRow) and its own `per K:`
//! outermost as the binder nests it, so the row stands for the same classes (D28): a row whose key is NULL is in none.
//! A written clause's variant signature splits its classes by the fields the clause reads (D70), which are not the
//! fields of a row the rewrite made of it: the row gets the signature of what it reads itself, built as the binder
//! builds one (none when it reads no field, so one instance per class). `source_alias` names the clause.
unique_ptr<Expression> ScopeRowPerClause(ClientContext &context, LogicalDecide &decide, unique_ptr<Expression> row,
                                         const DecideEnclosingScope &scope, const string &source_alias);

//! `row` once per instance of `scope`: under `per K:` for a scope keyed by K, under `per ():` for a query-wide one,
//! and as it is for one per row or for a range frame's helper, whose windows no key's classes name. A row that reads
//! only columns of `scope` is the same on every row of an instance.
unique_ptr<Expression> ScopeRowPerInstance(const LogicalDecide &decide, unique_ptr<Expression> row,
                                           const DecideVarScopeInfo &scope);

//! A lower-envelope row the tree states for a recorded ABS (LogicalDecide::AbsFunction), under its F node: `aux >=
//! operand`, or `aux >= -operand` when `negated`, on the rows the ABS is read on, once per instance of the auxiliary's
//! scope, attributed to the clause the ABS was written in. The emitter evaluates each per row, and stage 06 states
//! every row of the ABS from them (EmitAbsEnvelopes, EmitAbsConstraints).
unique_ptr<Expression> MakeAbsRow(ClientContext &context, const LogicalDecide &decide,
                                  const LogicalDecide::AbsFunction &function, bool negated);

//! What the way down to a node passed through, besides the sign it carries (WalkPolarity).
struct PolarityPath {
	//! A decision multiplies or divides the node, held or not (`b * sum(x)`).
	bool beside_decision = false;
	//! Another held node multiplies or divides it (`sum(x) * sum(y)`): the factor's sign is its own, not known here.
	bool beside_held = false;
	//! A node with no sign discipline holds it (`abs(sum(x) - 2)`, a CASE): its sign is 0 below.
	bool inside_function = false;
};
using PolarityHolds = std::function<bool(const Expression &)>;
using PolarityVisit = std::function<bool(unique_ptr<Expression> &, int, const PolarityPath &)>;

//! Polarity analysis: the sign each subterm carries once a clause is read as one expression `E` under `E <op> 0`
//! (`LHS - RHS`, so a comparison walks its right side with the sign flipped), or the objective as `E` under its sense.
//! Walking `expr` with `sign`, a node keeps its parent's sign through `+`, casts and reducer bodies (SUM, AVG, MIN and
//! MAX never decrease when a member grows), flips it through `-` and a factor known negative before the query runs, and
//! gets 0 ("unknown") under a factor whose sign only the rows decide or any other node. A factor of exactly zero reads
//! as positive, and a BOOL decision as a factor is never negative (it is 0 or 1), so it keeps the sign of the term it
//! scales (PlanTimeSignOf, `booleans`).
//!
//! `holds(node)` says whether a subtree holds anything the caller wants; a subtree that does not is skipped, and a
//! factor that does not is read as a multiplier. `visit(slot, sign, path)` sees each held node first, through the slot
//! that owns it, and returns true when it has taken the node and its subtree (it may walk below itself, with any sign).
//! `path` says what else the way down to the node passed through. Every reader of a push reads it from this one walk:
//! the ABS classifier, the reducers a value rewrite replaces, the ranges over decisions, and the definition of
//! non-linear reducer bodies.
void WalkPolarity(ClientContext &context, const BooleanDecisions &booleans, unique_ptr<Expression> &expr, int sign,
                  const PolarityHolds &holds, const PolarityVisit &visit, const PolarityPath &path = PolarityPath());

//! Allocate an auxiliary decision column `name` of `type`: its reference in
//! `decide_variables`, its Boolean flag, and its scope (the binder gives every declared decision one, so
//! `variable_scopes` stays parallel to `decide_variables`). A keyed auxiliary is one of its keyed scope's
//! `scoped_variable_indices`. `label` names the column in a diagnosis (F6) by what it stands for. Returns the
//! column's index.
idx_t AddAuxiliaryDecision(LogicalDecide &decide, const string &name, const LogicalType &type, bool is_boolean,
                           DecideVarScopeInfo scope, const string &label);

//! The scope of a column defined by two others (a product's auxiliary): their least upper bound, as DuckDB promotes
//! the type of a binary operator. Two query-wide scopes join to a query-wide one, a query-wide scope and a keyed one
//! to the keyed one, two of the same keyed scope to it, and anything else to a column per row.
DecideVarScopeInfo JoinScopes(const DecideVarScopeInfo &a, const DecideVarScopeInfo &b);

//! The scope of a value computed from the leaves `expr` reads: JoinScopes folded over them. A decision brings its
//! declared scope, a value `canonicalizer` finds query-wide (IsQueryWideConstant) is query-wide, and any other column,
//! or a reducer, is row data.
DecideVarScopeInfo ScopeOf(const LogicalDecide &decide, const DecideCanonicalizer &canonicalizer,
                           const Expression &expr);

//! A reference to decision column `var_idx`, as an expression of the DECIDE clause.
unique_ptr<Expression> MakeDecisionRef(const LogicalDecide &decide, idx_t var_idx);

//! The helper decision `var_idx` in place of the construct `replaced` it stands for (an ABS, a reducer's value), cast
//! to that construct's type as the binder would cast it. The clause stays a tree DuckDB can evaluate: the bound
//! classifier evaluates it with the decision replaced by a value (DecideReadSingletonRow).
void ReplaceWithDecision(ClientContext &context, const LogicalDecide &decide, idx_t var_idx,
                         unique_ptr<Expression> &replaced);

//! An expression as a plan prints it (RenderDecideSource, columns unqualified): the label
//! of an auxiliary decision, the text of a rewrite account.
string RenderForPlan(const Expression &expr, const LogicalDecide &decide);

//! The id of the source clause a written comparison carries (its `__source_clause_` tag). Every comparison of the
//! user's constraints is tagged when the binder registers the sources, so a stage-05 refusal quotes its clause
//! through QuoteWrittenClause (qualifier included) rather than rendering the comparison.
idx_t SourceClauseOf(const Expression &comparison, const LogicalDecide &decide);

//! Record what a rewrite made of `subject` (as RenderForPlan prints it), for EXPLAIN
//! (LogicalDecide::rewrite_accounts): under the clause `source_clause_id`, or the
//! objective (INVALID_INDEX). The alias form reads the clause id off a clause's alias;
//! an alias without one is the objective's.
void AddRewriteAccount(LogicalDecide &decide, idx_t source_clause_id, LogicalDecide::RewriteAccount::Kind kind,
                       string subject);
void AddRewriteAccount(LogicalDecide &decide, const string &clause_alias, LogicalDecide::RewriteAccount::Kind kind,
                       string subject);

//! The sides a helper decision holding `agg_name` (a reducer, or a range frame's reducer) pushed `push` needs: its
//! envelope (`value >= members` for SUM, AVG and MAX, the extremum link's envelope) when pushed its cheap way, its
//! closing side the other way, both under NONE (ReducerValueLink::need_envelope, need_closing).
void DecideHelperSides(const string &agg_name, ExtremumPush push, bool &need_envelope, bool &need_closing);

//! The helper decision holding `link`'s value, appended to the plan's (LogicalDecide::reducer_value_links), its floor
//! free: whole-numbered when `whole`; one instance per class of `scope` -- a copy of which becomes the helper's own,
//! naming the link; a `by ()` key is one class, or none when no row holds a member, so a read of it skips its
//! instance (D68) --; named by `link.label` in a plan and a diagnosis, and under `account` in EXPLAIN for the clause
//! `source_clause_id`. Returns the link's index.
idx_t AddValueHelper(LogicalDecide &decide, LogicalDecide::ReducerValueLink link, const KeyedScopeInfo &scope,
                     bool whole, idx_t source_clause_id, LogicalDecide::RewriteAccount::Kind account);

} // namespace decide_rewrite
} // namespace duckdb
