//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/planner/decide/decide_source_provenance.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/planner/decide/decide_tree_nodes.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

class BindContext;
class BoundDecideReducerExpression;
class BoundConjunctionExpression;
struct DecideTree;

//! The location rule (Clang's TreeTransform): a replacement takes the query location of exactly the node it equals in
//! value, so a message quotes it as the text the user wrote for that node (`DecideSourceText`). A node whose value
//! differs from every node it replaces takes none, and a render prints its operator and quotes its children. Every
//! site that builds a node in place of another calls this; an edit that changes a located node's value in place
//! clears its location instead.
inline void DecideReplaces(Expression &replacement, const Expression &replaced) {
	replacement.SetQueryLocation(replaced.GetQueryLocation());
}

//! Give every bound clause its stable source clause (DecideRowOrigin::source_clause_id) before planning rewrites them.
//! `keys` lets a reducer print its
//! `per K` and `by (...)`; every renderer below takes it for the same reason.
//! Each clause gets its span (`ConstraintSourceInfo::location`); `text` is the query's text, which renders the sides of
//! a clause whose sides canonicalization merges, and an IN. `frames` gives each frame over decisions its spelling
//! (DecideFrameSpec::spelling).
vector<ConstraintSourceInfo> InitializeConstraintSourceInfo(vector<BoundDecideClause> &clauses,
                                                            const DecideSourceText &text,
                                                            const vector<KeyedScopeInfo> &keyed_scopes,
                                                            idx_t decide_index, const vector<DecideKey> *keys,
                                                            const vector<DecideFrameSpec> &frames);

//! Render each clause's canonical sides after PlanSubqueries and canonicalization. `decide_index` tells a decision
//! from data: the plan layer reprints the decision structure and quotes the data.
//! `frames` gives each frame over decisions its spelling (DecideFrameSpec::spelling). `computed_values` tells a bound
//! holding a flattened subquery, which a message states as its evaluated number.
void FinalizeConstraintSourceInfo(const vector<BoundDecideClause> &clauses, vector<ConstraintSourceInfo> &sources,
                                  const DecideSourceText &text, const vector<KeyedScopeInfo> &keyed_scopes,
                                  idx_t decide_index, const vector<DecideKey> *keys,
                                  const vector<DecideFrameSpec> &frames,
                                  const vector<DecideComputedValue> &computed_values);

//! Render one bound expression as the user wrote it.
//!
//! This is the single user-facing renderer: `Expression::ToString()` prints the tree
//! as BOUND, so it shows the casts DuckDB inserted while reconciling types and spells
//! arithmetic as `"-"("*"(a, b), c)`. A node's query location names the span of the
//! query the user wrote for it (`DecideSourceText`, the Clang SourceManager idea), so a
//! located node is quoted instead of reprinted, and an implicit cast, which takes its
//! child's location, quotes as its child does. A node with no location (one an
//! optimizer rewrite made) prints its operator and quotes its children.
//!
//! Every surface that echoes a DECIDE clause back to a user goes through here:
//! EXPLAIN on both plans, and the clause labels an infeasibility diagnosis prints.
string RenderDecideSource(const Expression &expr, const DecideSourceText &text,
                          const vector<KeyedScopeInfo> &keyed_scopes, const vector<DecideKey> *keys = nullptr);

//! The value a NULL guard (NULL_GUARD_TAG) protects: the first operand of a COALESCE, the THEN of a CASE. An expression
//! that is no guard is its own value. What a guard protects is what a message names and what decides whether the value
//! is a constant: the guard itself reads a column.
const Expression &DecideUnguarded(const Expression &expr);

//! Whether `expr` is a NULL guard (NULL_GUARD_TAG), so DecideUnguarded reads through it.
bool DecideIsGuarded(const Expression &expr);

//! What a message about a clause renders with: the query's text and the registries above. A column reads as the user
//! wrote it (`D.depotID`): its bound reference is located.
struct DecideSourceNames {
	const DecideSourceText &text;
	const vector<KeyedScopeInfo> &keyed_scopes;
	const vector<DecideKey> &keys;
	//! The decision columns that stand for an expression (`LogicalDecide::aux_var_expressions`:
	//! an ABS auxiliary `abs(e)`, a product `(b * x)`, an indicator `(x = v)`, a helper
	//! `sum(e) by (g)`), which print as that expression rather than by their internal
	//! name; `decide_index` tells a decision column apart. Null prints the name.
	idx_t decide_index = DConstants::INVALID_INDEX;
	const vector<pair<idx_t, string>> *aux_labels = nullptr;
	//! True quotes every located node as the user wrote it, as a message does. False (a plan's layers) quotes the
	//! located nodes that read no decision and reprints the decision structure, which canonicalization and the
	//! rewrites reshape. DecidePlanNames makes the second kind.
	bool written = true;
	//! The frame selectors (LogicalDecide::frames): a frame marker the render does not quote prints its spelling
	//! (DecideFrameSpec::spelling). Null prints the marker's aggregate.
	const vector<DecideFrameSpec> *frames = nullptr;
};

//! The names a plan and the rewrites render with: the clause registries, the auxiliary labels and the frames. They
//! render as a plan's layers do (DecideSourceNames::written is false).
DecideSourceNames DecidePlanNames(const DecideSourceText &text, const vector<KeyedScopeInfo> &keyed_scopes,
                                  const vector<DecideKey> &keys, idx_t decide_index,
                                  const vector<pair<idx_t, string>> &aux_labels, const vector<DecideFrameSpec> &frames);

//! An expression as the query writes it (lower-case names, a reducer's `when`,
//! `per` and `by`), every column qualified by its relation when `relations` lists any. `parent_precedence` is how
//! tightly the operator it is an operand of binds (20 for `*`, 21 for the right of `/`): a text that binds looser is
//! parenthesized.
string RenderDecideSource(const Expression &expr, const DecideSourceNames &names, int parent_precedence = 0);

//! The body `e` of a reducer canonicalization built from `norm(e, p)` (DecideWrittenNorm), read back out of
//! its definition (DecideCanonicalizer::DesugarNormsInPlace); null for any other reducer. Every reader of a written
//! norm's body goes through here, so a new definition changes one function.
const Expression *DecideWrittenNormBody(const BoundDecideReducerExpression &reducer);

//! The body of the definition `reducer` holds of the norm `norm`, or nullptr with no order.
const Expression *DecideNormBody(const BoundDecideReducerExpression &reducer, const DecideNorm &norm);

//! The key of a clause's `per`, as written (`D.region`, `()`): cut from the query where the `per` stands (`location`),
//! else the key as it was first written (`keys`).
string RenderDecidePerKey(idx_t key, optional_idx location, const DecideSourceText &text,
                          const vector<DecideKey> *keys);

//! The body of a clause as a message quotes it (RenderDecideSource). A BETWEEN half carries the BETWEEN's location but
//! is half of its value, so it is reprinted, not quoted by that span.
string RenderDecideClauseBody(const BoundDecideClause &clause, const DecideSourceNames &names);

//! The objective as one display string, in the syntax the user wrote: `when c:` in front. Used by the logical and
//! physical EXPLAIN paths and by the planner's snapshots.
//! `written_sense` renders it in the direction it was written: the bound objective is a minimization, so a MAXIMIZE
//! objective reads with the sign of each of its terms reversed (DecideCanonicalizer::CanonicalizeObjective), as the
//! layers of a plan show the user's own sense and not the flip.
string RenderDecideObjective(const BoundDecideClause &objective, const DecideSourceNames &names,
                             bool written_sense = false);

//! The `when c:` an objective was written with, in front of it; empty for none.
string DecideObjectiveQualifier(const BoundDecideClause &objective, const DecideSourceNames &names);

//! A row an F node of the tree owns (an ABS envelope, an L0 link), as the solver receives it, with the clause it came
//! from (INVALID_INDEX for the objective's). `text` has no qualifier: CollectDecideClauseLayers puts the one its
//! clause was written with in front.
struct DecideFunctionRow {
	idx_t source_clause_id;
	string text;
};

//! The rows the tree's F nodes own, read from their clause roots: each root's terms and bound, as stage 05 lowered
//! them, without the zero terms the rewrite added. A plan reads them here because EXPLAIN plans without solving and the
//! rewrites keep these rows in the tree, not in the constraint list. `decide_variables` names each decision a term
//! reads.
vector<DecideFunctionRow> CollectDecideFunctionRows(const DecideTree &tree, const DecideSourceNames &names,
                                                    const vector<unique_ptr<Expression>> &decide_variables);

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
	//! across the comparison. A BETWEEN lists its two halves.
	vector<string> canonical;
	//! The rows the solver actually receives. Empty when they read the same as the
	//! layer above; more than one entry when a formulation expanded one clause into
	//! several rows.
	vector<string> rewritten;
};

//! Split the post-optimizer clause rows into per-clause layers.
//!
//! Association runs on `source_clause_id`, which the binder stamps on every written
//! comparison and the optimizer copies onto the rows it emits. The mapping is not
//! one-to-one in either direction and the result tolerates both: a linearized clause
//! owns several rows, a clause the optimizer lifted out of the tree owns none, and a row carrying no id at all is
//! returned in `unattributed`.
//! `accounts` (LogicalDecide::rewrite_accounts) are printed under their clause before its
//! rows, and `function_rows` (CollectDecideFunctionRows) follow the rows of the constraint list, each behind the
//! qualifier its clause was written with; the objective's (INVALID_INDEX), behind `objective_qualifier`
//! (DecideObjectiveQualifier), are left to RenderDecideObjectiveLayers, and so are the rows a rewrite added for the
//! objective (DecideRowOrigin::objective), returned in `objective_rows`.
vector<DecideClauseLayers>
CollectDecideClauseLayers(const vector<BoundDecideClause> &clauses, const DecideSourceNames &names,
                          const vector<ConstraintSourceInfo> &sources, vector<string> &unattributed,
                          vector<string> &objective_rows, const vector<LogicalDecide::RewriteAccount> &accounts,
                          const vector<DecideFunctionRow> &function_rows, const string &objective_qualifier);

//! One account line: the construct a rewrite replaced and what it became, in the one
//! vocabulary every plan uses ("`abs(e)` becomes a helper decision").
string RenderDecideRewriteAccount(const LogicalDecide::RewriteAccount &account);

//! Format clause layers as the indented block EXPLAIN prints, one clause per group.
string RenderDecideClauseLayers(const vector<DecideClauseLayers> &layers, const vector<string> &unattributed);

//! Format the objective's layers the same way. `written`, `canonical` and `canonical_differs` come from
//! LogicalDecide's two objective snapshots; `accounts` are what its rewrites made of it;
//! `rewritten` is the post-optimizer render; `derived_rows` are the rows its rewrites
//! added (CollectDecideClauseLayers' `objective_rows`).
string RenderDecideObjectiveLayers(const string &sense_prefix, const string &written, const string &canonical,
                                   bool canonical_differs, const vector<string> &rewritten,
                                   const vector<string> &accounts = {}, const vector<string> &derived_rows = {});

//! What the EXPLAIN parameters of a DECIDE operator are printed from: the decisions, their scopes, keys and frames, and
//! the clauses with their text and their sources. LogicalDecide holds them in its BoundDecide, PhysicalDecide in its
//! tree and its display.
struct DecidePlanParams {
	const vector<unique_ptr<Expression>> &variables;
	idx_t num_auxiliary_vars;
	const vector<KeyedScopeInfo> &keyed_scopes;
	const vector<DecideKey> &keys;
	const vector<DecideFrameSpec> &frames;
	DecideSense sense;
	idx_t decide_index;
	const DecideSourceText &source_text;
	const vector<pair<idx_t, string>> &aux_var_expressions;
	const vector<BoundDecideClause> &clauses;
	const vector<ConstraintSourceInfo> &constraint_sources;
	//! Its body is null when no objective is written
	const BoundDecideClause &objective;
	const string &written_objective;
	const string &canonical_objective;
	bool objective_canonical_differs;
};

//! The EXPLAIN parameters of a DECIDE operator, without the cardinality line: the variables, the objective and the
//! constraints, each clause's `when`/`per` printed in front as written. LogicalDecide::ParamsToString and
//! PhysicalDecide::ParamsToString both call it and add their own cardinality. Two facts each operator knows its own way
//! are its parameters: the rewrite `accounts` (the physical operator's add what the data made of each clause, which
//! EXPLAIN ANALYZE prints) and the `tree` itself, whose F nodes own the rows its rewrites added (the logical operator
//! has none).
inline InsertionOrderPreservingMap<string> RenderDecideParams(const DecidePlanParams &plan,
                                                              const vector<LogicalDecide::RewriteAccount> &accounts,
                                                              const DecideTree *tree) {
	InsertionOrderPreservingMap<string> result;

	// Variables (exclude auxiliary variables)
	string vars_info;
	idx_t user_var_count = plan.variables.size() - plan.num_auxiliary_vars;
	for (idx_t i = 0; i < user_var_count; i++) {
		if (i > 0) {
			vars_info += "\n";
		}
		vars_info += plan.variables[i]->GetName();
	}
	result["Variables"] = vars_info;

	// What the optimizer made of the clauses: a column that stands for an expression prints it, and each rewrite's
	// account follows its clause, the objective's its own.
	auto names = DecidePlanNames(plan.source_text, plan.keyed_scopes, plan.keys, plan.decide_index,
	                             plan.aux_var_expressions, plan.frames);
	vector<string> objective_accounts;
	for (auto &account : accounts) {
		if (account.source_clause_id == DConstants::INVALID_INDEX) {
			objective_accounts.push_back(RenderDecideRewriteAccount(account));
		}
	}
	// Constraints first: they also hand back the rows the rewrites added for the objective.
	vector<string> unattributed;
	vector<string> objective_rows;
	vector<DecideFunctionRow> function_rows;
	if (tree) {
		function_rows = CollectDecideFunctionRows(*tree, names, plan.variables);
	}
	// The parser requires a SUCH THAT clause, so a plan always has constraints (transform_select_node.cpp).
	const string objective_qualifier = plan.objective.body ? DecideObjectiveQualifier(plan.objective, names) : string();
	auto layers = CollectDecideClauseLayers(plan.clauses, names, plan.constraint_sources, unattributed, objective_rows,
	                                        accounts, function_rows, objective_qualifier);
	string constraints_text = RenderDecideClauseLayers(layers, unattributed);
	if (plan.objective.body) {
		const bool maximize = plan.sense == DecideSense::MAXIMIZE;
		string sense_prefix = maximize ? "MAXIMIZE " : "MINIMIZE ";
		vector<string> objective_strs {RenderDecideObjective(plan.objective, names, maximize)};
		result["Objective"] = RenderDecideObjectiveLayers(sense_prefix, plan.written_objective,
		                                                  plan.canonical_objective, plan.objective_canonical_differs,
		                                                  objective_strs, objective_accounts, objective_rows);
	} else {
		result["Objective"] = "FEASIBILITY";
	}
	result["Constraints"] = constraints_text;
	return result;
}

} // namespace duckdb
