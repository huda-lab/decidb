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

//! Constant-fold `expr` to a double at plan time, returning false instead of
//! throwing when it is not foldable.
bool TryEvaluateFoldableDoubleNoThrow(ClientContext &context, const Expression &expr, double &out);

//! Sign of a scale factor resolved at plan time: -1, +1, or 0 when unknown.
int ScaleSignAtPlanTime(ClientContext &context, const Expression *scale, bool divides);

//! Mark the ABS nodes that need a Big-M envelope. Must run before RewriteAbs.
void TagAbsConstraintsForBigM(ClientContext &context, LogicalDecide &decide);

} // namespace decide_rewrite
} // namespace duckdb
