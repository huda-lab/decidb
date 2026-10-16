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
#include "duckdb/planner/expression.hpp"

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

//! The clause a rewrite stands in, when it is written with the scope spellings. A row the
//! rewrite adds for it keeps the clause's rules and its `when`, so it reads only the rows
//! the clause keeps (anr_language_extension.md §8). `when` is null for none.
struct ClauseScope {
	bool scoped = false;
	const Expression *when = nullptr;
};

//! The clause scope in force below `node`, given the one above it: a scoped WHEN wrapper
//! supplies its condition, and a scoped wrapper, comparison or IN marks the clause.
ClauseScope EnterClauseScope(const Expression &node, ClauseScope scope);

//! `row`, which a rewrite adds for a clause in `scope`: tagged SCOPED_CLAUSE_TAG and
//! wrapped in a copy of the clause's `when` when the clause is scoped, else unchanged.
unique_ptr<Expression> ScopeHelperRow(unique_ptr<Expression> row, const ClauseScope &scope);

//! Mark the ABS nodes that need a Big-M envelope. Must run before RewriteAbs.
void TagAbsConstraintsForBigM(ClientContext &context, LogicalDecide &decide);

} // namespace decide_rewrite
} // namespace duckdb
