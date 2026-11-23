//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/optimizer/decide/decide_optimizer_internal.hpp
//
// Helpers shared between the DECIDE rewrite passes, which are split across
// decide_optimizer.cpp and its decide_rewrite_*.cpp siblings. Internal to
// layer 5 — nothing outside src/optimizer/decide/ includes this. The nested
// namespace keeps deliberately short names (StrToAggType, MakeFormulationRow,
// ScopeOf) out of `duckdb`; each rewrite file pulls it in with a
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
class DecideCanonicalizer;
class DecideTermSplitter;
class LogicalDecide;
class Optimizer;

namespace decide_rewrite {

//! Adds `check`, a message that raises the error of a value the model cannot read (VALUE_IS_VALID), to the DECIDE
//! node's scope checks, unless a check of the same value stands there. Two checks are of one value when they are equal
//! but for where each points (the place of a NaN or infinite value's error), so a value shared by two clauses is
//! checked once, and the place the first raises at stays (the first error reached is the one raised).
void AddValueCheck(LogicalDecide &decide, unique_ptr<Expression> check);

//! The objective nest `sum([when c] per K: inner([when c'] e) by (G))` read as terms: `inner` over every row the two
//! `when`s keep that lies in a G group. Null when `outer` is no such nest (RewriteScopedReducers states the shape).
unique_ptr<BoundDecideReducerExpression> ReadNestInPlace(const LogicalDecide &decide,
                                                         const BoundDecideReducerExpression &outer);

//! `SUM(body)` bound as a reducer, reading the rows `filter` keeps (null: every row).
unique_ptr<Expression> BindSumOver(Optimizer &optimizer, unique_ptr<Expression> body, const Expression *filter);

//! A row this layer emits (rather than one the user wrote) with the body `body`: structural, and of the clause
//! `origin`, so diagnostics still name the clause the user typed. It has no scope.
BoundDecideClause MakeFormulationRow(unique_ptr<Expression> body, const DecideRowOrigin &origin);

//! Puts `row`, which a rewrite adds for `clause`, in the clause's scope: a copy of its `when`, so the row reads only
//! the rows the clause keeps, and its own `per K:`, so the row stands for the same classes (D28): a row whose key is
//! NULL is in none. A written clause's variant signature splits its classes by the fields the clause reads (D70),
//! which are not the fields of a row the rewrite made of it: the row gets the signature of what it reads itself, built
//! as the canonicalizer builds one (none when it reads no field, so one instance per class).
void ScopeRowPerClause(ClientContext &context, LogicalDecide &decide, BoundDecideClause &row,
                       const BoundDecideClause &clause);

//! Makes `row` one per instance of `scope`: `per K:` for a scope keyed by K, `per ():` for a query-wide one, and as it
//! is for one per row or for a range frame's helper, whose windows no key's classes name. A row that reads only
//! columns of `scope` is the same on every row of an instance. A row that has its clause's `per` keeps it.
void ScopeRowPerInstance(const LogicalDecide &decide, BoundDecideClause &row, const DecideVarScopeInfo &scope);

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

//! Whether the bilinear rewrite reads `body`: every body but a range frame's value link, whose payload is held as a
//! frame reads it.
bool BilinearReadsBody(const LogicalDecide &decide, const DecideOwnedBody &body);

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

//! The id of the source clause of a written clause. Every clause the user wrote is given one when the binder registers
//! the sources, so a stage-05 refusal quotes its clause through QuoteWrittenClause (qualifier included) rather than
//! rendering the comparison.
idx_t SourceClauseOf(const BoundDecideClause &clause, const LogicalDecide &decide);

//! Refuses `operand`, which `construct` (`<>`, ABS) reads as a linear form, when it holds a product or a power of
//! decisions. The refusal is the construct's: `at` is the expression the caret points at. It runs after the bilinear
//! rewrite has kept the products with a BOOL factor, which are linear through their helper and stay accepted; a
//! product it leaves has no formulation, and dropping its terms would change the answer.
void RequireLinearOperand(const DecideTermSplitter &splitter, const Expression &operand, const Expression &at,
                          const string &construct);

//! Record what a rewrite made of `subject` (as RenderForPlan prints it), for EXPLAIN
//! (LogicalDecide::rewrite_accounts): under the clause `source_clause_id`, or the
//! objective (INVALID_INDEX).
void AddRewriteAccount(LogicalDecide &decide, idx_t source_clause_id, LogicalDecide::RewriteAccount::Kind kind,
                       string subject);

//! The helper decision holding `link`'s value, appended to the plan's (LogicalDecide::helper_definitions), its floor
//! free: whole-numbered when `whole`; one instance per class of `scope` -- a copy of which becomes the helper's own,
//! naming the link; a `by ()` key is one class, or none when no row holds a member, so a read of it skips its
//! instance (D68) --; named by `link->label` in a plan and a diagnosis, and under `account` in EXPLAIN for the clause
//! `source_clause_id`. Returns the link's index.
idx_t AddValueHelper(LogicalDecide &decide, unique_ptr<DecideReducerValueDefinition> link, const KeyedScopeInfo &scope,
                     bool whole, idx_t source_clause_id, LogicalDecide::RewriteAccount::Kind account);

} // namespace decide_rewrite
} // namespace duckdb
