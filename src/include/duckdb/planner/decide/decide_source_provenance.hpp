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
//! `out_source_ids`, when supplied, receives the source_clause_id behind each emitted
//! string -- DConstants::INVALID_INDEX for a row no clause claims. This is what lets a
//! caller group the post-optimizer rows under the clause they came from.
void CollectDecideExpressionStrings(const Expression &expr, const vector<string> &fragments,
                                    const vector<EntityScopeInfo> &entity_scopes, vector<string> &out,
                                    const vector<ConstraintSourceInfo> *sources = nullptr,
                                    vector<idx_t> *out_source_ids = nullptr);

//! Render a whole objective tree as one display string.
//!
//! Not RenderDecideSource: an objective carrying a WHEN wrapper is a tagged conjunction,
//! which the leaf renderer spells `SUM(x) AND cond` rather than the postfix
//! `SUM(x) WHEN cond` the user wrote. Only the clause walker unwraps those tags, so the
//! objective's snapshots go through it too.
string RenderDecideObjective(const Expression &expr, const vector<string> &fragments,
                             const vector<EntityScopeInfo> &entity_scopes);

//! One user clause, rendered at each layer that says something different about it.
//!
//! A DECIDE clause is read three times on its way to a solver: as the user wrote it, as
//! canonicalization shaped it (decisions left, bound right), and as the optimizer
//! formulated it. EXPLAIN printed only the last of those, so a clause that became a
//! Big-M encoding, an auxiliary variable, or nothing at all looked the same as one that
//! passed through untouched. These three fields keep the layers apart so a plan can show
//! what became of each clause.
struct DecideClauseLayers {
	//! The clause as written, WHEN/PER qualifiers included. Always set.
	string written;
	//! The canonical form. Empty when it reads the same as `written`, which is the
	//! common case -- canonicalization only shows up when it had to move a decision
	//! across the comparison.
	string canonical;
	//! The rows the solver actually receives. Empty when they read the same as the
	//! layer above; more than one entry when a formulation expanded one clause into
	//! several rows.
	vector<string> rewritten;
};

//! Split the post-optimizer constraint tree into per-clause layers.
//!
//! Association runs on `source_clause_id`, which the binder stamps on every written
//! comparison and the optimizer copies onto the rows it emits. The mapping is not
//! one-to-one in either direction and the result tolerates both: a linearized clause
//! owns several rows, a clause the optimizer extracted into `composed_minmax_constraints`
//! owns none in the tree, and a row carrying no id at all is returned in `unattributed`.
vector<DecideClauseLayers> CollectDecideClauseLayers(
    optional_ptr<const Expression> constraints,
    const vector<LogicalDecide::ComposedMinMaxConstraint> &composed_minmax_constraints,
    const vector<string> &fragments, const vector<EntityScopeInfo> &entity_scopes,
    const vector<ConstraintSourceInfo> &sources, vector<string> &unattributed);

//! Format clause layers as the indented block EXPLAIN prints, one clause per group.
string RenderDecideClauseLayers(const vector<DecideClauseLayers> &layers,
                                const vector<string> &unattributed);

//! Format the objective's layers the same way. `written` and `canonical` come from
//! LogicalDecide's two objective snapshots; `rewritten` is the post-optimizer render.
string RenderDecideObjectiveLayers(const string &sense_prefix, const string &written,
                                   const string &canonical, const vector<string> &rewritten);

} // namespace duckdb
