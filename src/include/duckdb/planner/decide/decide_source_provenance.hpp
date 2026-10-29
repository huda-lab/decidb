//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/decide/decide_source_provenance.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

class BindContext;
class BoundAggregateExpression;
class BoundConjunctionExpression;

//! Tag source-only casts and subqueries before binding obscures their spelling.
void TagDecideSourceFragments(ParsedExpression &expr, vector<string> &fragments);

//! Preserve a source-fragment tag when a DECIDE binder replaces a parsed node.
void PreserveDecideSourceFragment(const ParsedExpression &source, Expression &bound);

//! Assign stable source ids to bound comparisons before planning rewrites them.
//! `keys` lets a reducer print its
//! `per K` and `by (...)`; every renderer below takes it for the same reason.
//! `text`, when given, supplies each comparison's two sides as the user wrote them.
vector<ConstraintSourceInfo> InitializeConstraintSourceInfo(Expression &constraints, const vector<string> &fragments,
                                                            const vector<EntityScopeInfo> &entity_scopes,
                                                            idx_t decide_index, const vector<DecideKey> *keys = nullptr,
                                                            const DecideSourceText *text = nullptr);

//! Rebuild canonical display templates after PlanSubqueries and canonicalization.
void FinalizeConstraintSourceInfo(const Expression &constraints, vector<ConstraintSourceInfo> &sources,
                                  const vector<string> &fragments, const vector<EntityScopeInfo> &entity_scopes,
                                  const vector<DecideKey> *keys = nullptr);

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
                          const vector<EntityScopeInfo> &entity_scopes, const vector<DecideKey> *keys = nullptr);

//! What a message about a clause renders with: the registries above. A column reads as the user wrote it
//! (`D.depotID`): its bound reference carries that spelling as a source fragment.
struct DecideSourceNames {
    const vector<string> &fragments;
    const vector<EntityScopeInfo> &entity_scopes;
	const vector<DecideKey> &keys;
    //! The decision columns that stand for an expression (`LogicalDecide::aux_var_expressions`:
    //! an ABS auxiliary `abs(e)`, a product `(b * x)`, an indicator `(x = v)`, a helper
    //! `sum(e) by (g)`), which print as that expression rather than by their internal
    //! name; `decide_index` tells a decision column apart. Null prints the name.
    idx_t decide_index = DConstants::INVALID_INDEX;
    const vector<pair<idx_t, string>> *aux_labels = nullptr;
};

//! The names a plan and the rewrites render with: the clause registries and the auxiliary labels.
DecideSourceNames DecidePlanNames(const vector<string> &fragments, const vector<EntityScopeInfo> &entity_scopes,
                                  const vector<DecideKey> &keys, idx_t decide_index,
                                  const vector<pair<idx_t, string>> &aux_labels);

//! An expression as the query writes it (lower-case names, a reducer's `when`,
//! `per` and `by`), every column qualified by its relation when `relations` lists any.
string RenderDecideSource(const Expression &expr, const DecideSourceNames &names);

//! A reducer as the query writes it, without its `by`: `sum(when S.priority:
//! ship)`, for a message that names its `by` group separately.
string RenderDecideReducer(const BoundAggregateExpression &reducer, const DecideSourceNames &names);

//! The body `e` of a reducer canonicalization built from `norm(e, p)` (WRITTEN_NORM_TAG_PREFIX), read back out of
//! its definition (DecideCanonicalizer::DesugarNormsInPlace); null for any other reducer. Every reader of a written
//! norm's body goes through here, so a new definition changes one function.
const Expression *DecideWrittenNormBody(const BoundAggregateExpression &reducer);

//! The key of a `per` wrapper, as written (`D.region`, `()`).
string RenderDecidePerKey(const BoundConjunctionExpression &per, const vector<string> &fragments);

//! Split a DECIDE constraint or objective tree into one rendered string per clause,
//! turning the wrappers the binder stamps back into the syntax the user wrote: `when c
//! per K:` in front, and the two-level objective. Leaves are rendered by
//! RenderDecideSource. Used by the logical and
//! physical EXPLAIN paths, which see the tree AFTER the optimizer, so the output
//! includes the rows linearization emitted as well as the ones the user wrote.
//! `sources`, when supplied, lets a comparison that canonicalization rewrote print as
//! the user wrote it instead of as the algebra stage 04 produced. Optional so callers
//! without the registry keep the plain canonical rendering.
//! `out_source_ids`, when supplied, receives the source_clause_id behind each emitted
//! string -- DConstants::INVALID_INDEX for a row no clause claims. This is what lets a
//! caller group the post-optimizer rows under the clause they came from.
void CollectDecideExpressionStrings(const Expression &expr, const DecideSourceNames &names, vector<string> &out,
                                    const vector<ConstraintSourceInfo> *sources = nullptr,
                                    vector<idx_t> *out_source_ids = nullptr);

//! Render a whole objective tree as one display string.
//!
//! Not RenderDecideSource: an objective carrying a `when` wrapper is a tagged
//! conjunction, which the leaf renderer spells `sum(x) AND cond` rather than the
//! `when cond: sum(x)` the user wrote. Only the clause walker unwraps those tags, so the
//! objective's snapshots go through it too.
string RenderDecideObjective(const Expression &expr, const DecideSourceNames &names);

//! One user clause, rendered at each layer that says something different about it.
//!
//! A DECIDE clause is read three times on its way to a solver: as the user wrote it, as
//! canonicalization shaped it (decisions left, bound right), and as the optimizer
//! formulated it. EXPLAIN printed only the last of those, so a clause that became a
//! Big-M encoding, an auxiliary variable, or nothing at all looked the same as one that
//! passed through untouched. These three fields keep the layers apart so a plan can show
//! what became of each clause.
struct DecideClauseLayers {
	//! The clause as written, `when`/`per` qualifiers included. Always set.
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
//! `accounts` (LogicalDecide::rewrite_accounts) are printed under their clause before its
//! rows; the objective's (INVALID_INDEX) are left to RenderDecideObjectiveLayers, and so
//! are the rows a rewrite added for the objective (OBJECTIVE_ROW_TAG), returned in
//! `objective_rows`.
vector<DecideClauseLayers> CollectDecideClauseLayers(optional_ptr<const Expression> constraints,
                                                     const DecideSourceNames &names,
                                                     const vector<ConstraintSourceInfo> &sources,
                                                     vector<string> &unattributed, vector<string> &objective_rows,
                                                     const vector<LogicalDecide::RewriteAccount> &accounts);

//! One account line: the construct a rewrite replaced and what it became, in the one
//! vocabulary every plan uses ("`abs(e)` becomes an auxiliary variable").
string RenderDecideRewriteAccount(const LogicalDecide::RewriteAccount &account);

//! Format clause layers as the indented block EXPLAIN prints, one clause per group.
string RenderDecideClauseLayers(const vector<DecideClauseLayers> &layers,
                                const vector<string> &unattributed);

//! Format the objective's layers the same way. `written` and `canonical` come from
//! LogicalDecide's two objective snapshots; `accounts` are what its rewrites made of it;
//! `rewritten` is the post-optimizer render; `derived_rows` are the rows its rewrites
//! added (CollectDecideClauseLayers' `objective_rows`).
string RenderDecideObjectiveLayers(const string &sense_prefix, const string &written, const string &canonical,
                                   const vector<string> &rewritten, const vector<string> &accounts = {},
                                   const vector<string> &derived_rows = {});

} // namespace duckdb
