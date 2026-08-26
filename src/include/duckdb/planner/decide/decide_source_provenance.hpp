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
                                                            const vector<EntityScopeInfo> &entity_scopes,
                                                            idx_t decide_index);

//! Rebuild canonical display templates after PlanSubqueries and canonicalization.
void FinalizeConstraintSourceInfo(const Expression &constraints,
                                  vector<ConstraintSourceInfo> &sources,
                                  const vector<string> &fragments,
                                  const vector<EntityScopeInfo> &entity_scopes);

//! Render one bound expression as the user wrote it.
//!
//! This is the single user-facing renderer: `Expression::ToString()` prints the tree
//! as BOUND, so it shows the casts DuckDB inserted while reconciling types and spells
//! arithmetic as `"-"("*"(a, b), c)`. Authorship is what separates a cast the user
//! typed from one the binder added, and `TagDecideSourceFragments` recorded exactly
//! that before binding could obscure it -- so a tagged cast or subquery replays its
//! written spelling out of `fragments` and an untagged one is dropped.
//!
//! Every surface that echoes a DECIDE clause back to a user goes through here:
//! EXPLAIN on both plans, and the clause labels an infeasibility diagnosis prints.
string RenderDecideSource(const Expression &expr, const vector<string> &fragments,
                          const vector<EntityScopeInfo> &entity_scopes);

//! Split a DECIDE constraint or objective tree into one rendered string per clause,
//! turning the WHEN and PER wrappers the binder stamps back into the postfix syntax
//! the user wrote. Leaves are rendered by RenderDecideSource. Used by the logical and
//! physical EXPLAIN paths, which see the tree AFTER the optimizer, so the output
//! includes the rows linearization emitted as well as the ones the user wrote.
//! `sources`, when supplied, lets a comparison that canonicalization rewrote print as
//! the user wrote it instead of as the algebra stage 04 produced. Optional so callers
//! without the registry keep the plain canonical rendering.
void CollectDecideExpressionStrings(const Expression &expr, const vector<string> &fragments,
                                    const vector<EntityScopeInfo> &entity_scopes, vector<string> &out,
                                    const vector<ConstraintSourceInfo> *sources = nullptr);

} // namespace duckdb
