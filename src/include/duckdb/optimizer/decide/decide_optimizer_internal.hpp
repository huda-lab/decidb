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

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_reducer_path.hpp"
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

//! Mark `row`, a row a rewrite added for a construct inside a reducer over decisions, as one
//! that defines that reducer's members: MEMBER_ROW_TAG and each of `member_frames` (the
//! reducer's MEMBER_FRAME_TAG_PREFIX tags), one tag per call. It then reads the members' rows
//! alone and is never skipped with its clause's instances (DecideConstraint::defines_members).
void MarkMemberRow(Expression &row, const vector<idx_t> &member_frames);
//! The member frames a row added at `path` carries, through its innermost reducer; whether
//! it is a member row at all is `path.innermost` being set.
vector<idx_t> MemberFramesAt(const DecideReducerPath &path);

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

//! A reference to decision column `var_idx`, as an expression of the DECIDE clause.
unique_ptr<Expression> MakeDecisionRef(const LogicalDecide &decide, idx_t var_idx);

//! An expression as a plan prints it (RenderDecideSource, columns unqualified): the label
//! of an auxiliary decision, the text of a rewrite account.
string RenderForPlan(const Expression &expr, const LogicalDecide &decide);

//! Record what a rewrite made of `subject` (as RenderForPlan prints it), for EXPLAIN
//! (LogicalDecide::rewrite_accounts): under the clause `source_clause_id`, or the
//! objective (INVALID_INDEX). The alias form reads the clause id off a clause's alias;
//! an alias without one is the objective's.
void AddRewriteAccount(LogicalDecide &decide, idx_t source_clause_id, LogicalDecide::RewriteAccount::Kind kind,
                       string subject);
void AddRewriteAccount(LogicalDecide &decide, const string &clause_alias, LogicalDecide::RewriteAccount::Kind kind,
                       string subject);

//! A decision-bearing expression a rewrite replaces by a helper decision -- a reducer, or a
//! range frame over decisions (syntax_reference.md §5.6) -- and the sign its term carries
//! once the side it stands in is read as a sum: through `+`, `-`, binder casts and data
//! factors of known sign; 0 where that sign is not known before the query runs (a factor
//! that is a data column or a subquery, anything else around it), which pins a MIN/MAX
//! helper from both sides.
struct DecisionSite {
	unique_ptr<Expression> *slot;
	int sign;
};
enum class DecisionSiteKind : uint8_t {
	//! The value rewrite's: the reducers of a side, outside every reducer (it walks a
	//! reducer's argument at that reducer, along its path); a frame is skipped.
	REDUCERS,
	//! The range rewrite's: every range over decisions, through the members of a SUM or AVG
	//! (whose sign they keep) and under anything else (sign 0; under a MIN or MAX, D73).
	RANGES
};
//! The sites of `kind` in `expr`, whose sign is `sign`, appended to `out`.
void CollectDecisionSites(unique_ptr<Expression> &expr, int sign, DecisionSiteKind kind, ClientContext &context,
                          const LogicalDecide &decide, vector<DecisionSite> &out);

//! The sides a helper holding a MIN or MAX (`agg_name`) pushed `push` needs
//! (ExtremumLinkSpec): its envelope when pushed its cheap way, its closing side the other
//! way, both under NONE; neither for any other reducer, whose definition is an equality.
void ExtremumSides(const string &agg_name, ExtremumPush push, bool &need_envelope, bool &need_closing);

//! The helper decision holding `link`'s value, appended to the plan's: whole-numbered when
//! `whole`, so a strict bound on a row reading it still steps on the integer lattice; one
//! instance per entity of `scope` (which names the link, and which execution builds from
//! it), or one for the query when `scope` is null; named, in a plan and a diagnosis, by
//! `link.label`, under `account` in EXPLAIN. Returns the link's index.
idx_t AddValueHelper(LogicalDecide &decide, LogicalDecide::ReducerValueLink link, unique_ptr<EntityScopeInfo> scope,
                     bool whole, LogicalDecide::RewriteAccount::Kind account);

} // namespace decide_rewrite
} // namespace duckdb
