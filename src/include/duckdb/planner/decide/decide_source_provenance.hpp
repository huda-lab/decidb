//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/decide/decide_source_provenance.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"

namespace duckdb {

//! Tag source-only casts and subqueries before binding obscures their spelling.
void TagDecideSourceFragments(ParsedExpression &expr, vector<string> &fragments);

//! Preserve a source-fragment tag when a DECIDE binder replaces a parsed node.
void PreserveDecideSourceFragment(const ParsedExpression &source, Expression &bound);

//! Assign stable source ids to bound comparisons before planning rewrites them.
vector<ConstraintSourceInfo> InitializeConstraintSourceInfo(Expression &constraints,
                                                            const vector<string> &fragments,
                                                            const vector<EntityScopeInfo> &entity_scopes);

//! Rebuild canonical display templates after PlanSubqueries and canonicalization.
void FinalizeConstraintSourceInfo(const Expression &constraints,
                                  vector<ConstraintSourceInfo> &sources,
                                  const vector<string> &fragments,
                                  const vector<EntityScopeInfo> &entity_scopes);

} // namespace duckdb
