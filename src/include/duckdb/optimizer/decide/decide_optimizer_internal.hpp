//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/optimizer/decide/decide_optimizer_internal.hpp
//
// Helpers shared between the DECIDE rewrite passes, which are split across
// decide_optimizer.cpp and its decide_rewrite_*.cpp siblings. Internal to
// layer 5 — nothing outside src/optimizer/decide/ includes this. The nested
// namespace keeps deliberately short names (StrToAggType, MakeTrueExpression,
// DescendSourceAlias) out of `duckdb`; each rewrite file pulls it in with a
// file-scope `using namespace decide_rewrite;`.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <functional>

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

class ClientContext;
class LogicalDecide;

namespace decide_rewrite {

//! Map an aggregate's SQL name onto the objective aggregate enum.
ObjectiveAggregateType StrToAggType(const string &name);

//! Recognize the binder's `norm` marker alias and return its payload.
bool TryParseNormMarker(const string &alias, string &payload);

//! Carry a source clause's provenance tags onto an expression a rewrite emitted
//! in its place, so diagnostics still name the clause the user typed.
void CopyClauseProvenanceTags(const string &from_alias, Expression &to);

//! Stamp an expression as formulation-generated (a row this layer emitted rather
//! than one the user wrote), attributed to `source_alias`.
void MarkFormulationConstraint(Expression &expr, const string &source_alias);

//! Walk down to the nearest source alias on `expr`, falling back to `inherited`.
string DescendSourceAlias(const Expression &expr, const string &inherited);

//! A TRUE placeholder standing in for a clause a rewrite has consumed.
unique_ptr<Expression> MakeTrueExpression(const string &source_alias = string());

//! The clause a rewrite stands in. A row the rewrite adds for it keeps the clause's
//! `when`, so it reads only the rows the clause keeps. `when` is null for none.
struct ClauseScope {
	const Expression *when = nullptr;
};

//! The clause scope in force below `node`, given the one above it: a `when` wrapper
//! supplies its condition.
ClauseScope EnterClauseScope(const Expression &node, ClauseScope scope);

//! `row`, which a rewrite adds for a clause in `scope`: wrapped in a copy of the clause's
//! `when` when it has one.
unique_ptr<Expression> ScopeHelperRow(unique_ptr<Expression> row, const ClauseScope &scope);

//! Polarity analysis: the sign each subterm carries once a clause is read as one expression `E` under `E <op> 0`
//! (`LHS - RHS`), or the objective as `E` under its sense. Walking `expr` with `sign`, a node keeps its parent's sign
//! through `+`, casts and reducer bodies (SUM, AVG, MIN and MAX never decrease when a member grows), flips it through
//! `-` and a factor known negative before the query runs, and gets 0 ("unknown") under a factor whose sign only the
//! rows decide or any other node. A term under a factor of exactly zero vanishes and is not walked.
//!
//! `holds(node)` says whether a subtree holds anything the caller wants; a subtree that does not is skipped, and a
//! factor that does not is read as a multiplier. `visit(node, sign)` sees each held node first and returns true when it
//! has taken the node and its subtree (it may walk below itself, with any sign). The ABS classifier and the definition
//! of non-linear reducer bodies read their direction from this one walk.
using PolarityHolds = std::function<bool(const Expression &)>;
using PolarityVisit = std::function<bool(Expression &, int)>;
void WalkPolarity(ClientContext &context, Expression &expr, int sign, const PolarityHolds &holds,
                  const PolarityVisit &visit);

//! Mark the ABS nodes, in the constraints and the objective, that need a Big-M
//! envelope: those whose sign under the relation or the sense does not push the
//! auxiliary down, or is not known until the query runs. Must run before RewriteAbs.
void TagAbsForBigM(ClientContext &context, LogicalDecide &decide);

//! Allocate an auxiliary decision column `name` of `type`: its reference in
//! `decide_variables`, its Boolean flag, and its scope -- recorded only when the plan
//! keeps scopes at all (`variable_scopes` is empty while every decision is row-scoped,
//! and stays so for a row-scoped auxiliary). A non-empty `label` names the column in a
//! diagnosis (F6) by what it stands for. Returns the column's index.
idx_t AddAuxiliaryDecision(LogicalDecide &decide, const string &name, const LogicalType &type, bool is_boolean,
                           DecideVarScopeInfo scope = DecideVarScopeInfo::Row(), const string &label = string());

//! The scope of a column defined by two others (a product's auxiliary): their least upper bound, as DuckDB promotes
//! the type of a binary operator. Two query-wide scopes join to a query-wide one, a query-wide scope and a keyed one
//! to the keyed one, two of the same keyed scope to it, and anything else to a column per row.
DecideVarScopeInfo JoinScopes(const DecideVarScopeInfo &a, const DecideVarScopeInfo &b);

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

} // namespace decide_rewrite
} // namespace duckdb
