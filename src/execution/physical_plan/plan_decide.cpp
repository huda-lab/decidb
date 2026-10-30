#include "duckdb/common/decide_profile.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/decidb/solver/ilp_solver.hpp"
#include "duckdb/optimizer/decide/decide_linear_form.hpp"
#include "duckdb/optimizer/decide/decide_solver_gate.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <functional>

namespace duckdb {

//! A column name as SQL: quoted only where SQL requires it. An unreserved keyword (`zone`) is a valid
//! column reference as it stands, so it stays bare.
static string QuoteColumnName(const string &name) {
	if (KeywordHelper::RequiresQuotes(name) &&
	    KeywordHelper::KeywordCategoryType(name) != KeywordCategory::KEYWORD_UNRESERVED) {
		return KeywordHelper::WriteQuoted(name, '"');
	}
	return name;
}

unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreatePlan(LogicalDecide &op) {
	DecideProfileScope profile("frontend.physical_decide");
	// Stage 05 carries meaning, not just speed: it chooses the backend, reduces each data bound to its group's tightest
	// value with its value checks, and folds bounds into column boxes. A plan it never saw (the optimizer switched off,
	// or PRAGMA enable_verification's unoptimized run) cannot be answered, so it is refused in SQL terms.
	if (!op.optimized) {
		throw InvalidInputException("DECIDE needs the DECIDE optimizer. Remove 'decide_optimizer' from "
		                            "disabled_optimizers, and run PRAGMA enable_optimizer if it is disabled.");
	}
	D_ASSERT(op.children.size() == 1);
	// Both clauses are verified after ALL optimizer rewriting and before any logical
	// expression is moved into the physical operator. The objective is checked here
	// for the same reason the constraints are: every rewrite that touches it now
	// re-enters through LogicalDecide::SetObjective, so a rewrite that breaks the
	// canonical form should fail loudly rather than reach the extractor.
	if (op.decide_constraints || op.decide_objective) {
        DecideCanonicalizer canonicalizer(context, op.decide_index, op.variable_scopes, op.keyed_scopes,
                                          op.is_boolean_var);
        if (op.decide_constraints) {
            canonicalizer.VerifyCanonical(*op.decide_constraints);
        }
        if (op.decide_objective) {
            canonicalizer.VerifyCanonicalObjective(*op.decide_objective);
        }
    }
    // Flatten the verified tree into the prepared linear form. The pass itself is
    // owned by stage 05 (src/optimizer/decide/decide_linear_form.cpp); it is TRIGGERED
    // here because this is the first point at which column bindings are final. The
    // prepared terms hold copies of the coefficient subtrees, and RemoveUnusedColumns,
    // ColumnLifetimeAnalyzer and late materialization all rebind the operator's own
    // expressions after the DECIDE optimizer runs -- copies taken earlier would keep
    // bindings that no longer name the right input columns.
    BuildDecidePreparedModel(context, op);
    // With the shape of every constraint and the objective settled, check the model
    // class this query needs against the backend chosen for it. A model class is a
    // gate, not an optimization -- nothing lowers a quadratic constraint into linear
    // rows -- so an unsupported one is refused here, before the query reads a row,
    // and the message blames the host rather than the query.
    RequireDecideSolverSupport(op);
    // Capture child column bindings BEFORE CreatePlan: several logical operators
    // (notably LogicalProjection) move their `expressions` vector into the physical
    // op during CreatePlan, which leaves GetColumnBindings() returning an empty
    // vector afterward. LogicalGet isn't affected because its bindings come from
    // column_ids, which is why base-table keyed scopes worked and subquery /
    // CTE-backed scopes silently collapsed to a single class.
    auto child_bindings = op.children[0]->GetColumnBindings();
    // Capture user-written column names off the child projection BEFORE CreatePlan
    // moves its `expressions` vector out (same reason as child_bindings above). These
    // back-fill names for columns that appear ONLY in the outer SELECT — never in the
    // DECIDE clause — so the unbounded diagnosis can characterize an escaping slice by
    // them. We keep only names the user actually wrote: an explicit `AS name` alias, or
    // a bare source-column reference. A computed projection expression with no alias
    // carries only a generated name (its ToString), which we deliberately drop so the
    // diagnosis never prints a label the user never typed.
    // Resolve each child column to the name the USER wrote for it, using the map the
    // binder captured from its BindContext (LogicalDecide::source_column_*). That is the
    // same table name resolution itself used, so every source kind is covered uniformly:
    // a base table's catalog names, a `t(a, b, c)` alias list over `(VALUES ...)`, a
    // subquery's or a CTE's output names. Reading names off the plan instead cannot work
    // — an alias list is recorded on the binding and never reaches the plan, whose
    // projection carries the binder's positional `col0` / `col1` placeholders.
    //
    // Captured BEFORE CreatePlan for the same reason as `child_bindings`: several
    // operators move their contents out during planning.
    //
    // The map is keyed by DecideSourceColumnKey: a scan's column by its table ordinal, any other by its binding.
    // Getting this wrong silently mislabels one column as another rather than failing.
    //
    // The binder's map answers "what is this column called", not "did the user write that
    // name": a projection expression with no alias is recorded under its own `ToString`, so
    // an unaliased computed column would be labelled `CASE WHEN ((i <= 25)) THEN ('A') ELSE
    // 'B' END`. The plan is the right place to judge that — it still holds the expression —
    // so the key names no column for it, while the spelling comes from the binder.
    vector<string> child_userwritten_names(child_bindings.size(), string());
    for (idx_t i = 0; i < child_bindings.size(); i++) {
        auto key = DecideSourceColumnKey(*op.children[0], child_bindings[i]);
        if (key.column_index == DConstants::INVALID_INDEX) {
            continue;
        }
        for (const auto &source_column : op.source_columns) {
            if (source_column.binding == key) {
                child_userwritten_names[i] = source_column.name;
                break;
            }
        }
    }
    auto child_plan = CreatePlan(*op.children[0]);
    auto decide_op = make_uniq<PhysicalDecide>(
        op.types, op.estimated_cardinality, std::move(child_plan),
        op.decide_index, std::move(op.decide_variables),
        std::move(op.decide_constraints), op.decide_sense, std::move(op.decide_objective));
    // The DIAGNOSE prefix, carried the last leg of parser → binder → plan → stage 08.
    decide_op->diagnose = op.diagnose;
    decide_op->solver_backend_name = op.solver_backend_name;
    decide_op->use_native_constructs = op.use_native_constructs;
    decide_op->force_native_constructs = op.force_native_constructs;
    decide_op->num_auxiliary_vars = op.num_auxiliary_vars;
    decide_op->is_boolean_var = op.is_boolean_var;
    decide_op->ne_clause_sources = std::move(op.ne_clause_sources);
    decide_op->minmax_clause_labels = std::move(op.minmax_clause_labels);
    decide_op->bilinear_links = std::move(op.bilinear_links);
    decide_op->abs_maximize_links = std::move(op.abs_maximize_links);
    decide_op->aux_var_expressions = std::move(op.aux_var_expressions);
    decide_op->constraint_sources = std::move(op.constraint_sources);
    decide_op->written_objective = std::move(op.written_objective);
    decide_op->canonical_objective = std::move(op.canonical_objective);
    decide_op->objective_constant_offset = op.objective_constant_offset;
    decide_op->source_fragments = std::move(op.source_fragments);
    decide_op->composed_minmax_constraints = std::move(op.composed_minmax_constraints);
    decide_op->composed_minmax_objective_terms = std::move(op.composed_minmax_objective_terms);
    decide_op->reducer_value_links = std::move(op.reducer_value_links);
    decide_op->rewrite_accounts = std::move(op.rewrite_accounts);
    decide_op->singleton_row_boxes = std::move(op.singleton_row_boxes);
    decide_op->user_singleton_row_bounds = std::move(op.user_singleton_row_bounds);
    decide_op->flat_objective_agg = op.flat_objective_agg;
    decide_op->flat_objective_is_easy = op.flat_objective_is_easy;
    decide_op->per_inner_agg = op.per_inner_agg;
    decide_op->per_outer_agg = op.per_outer_agg;
    decide_op->per_inner_is_easy = op.per_inner_is_easy;
    decide_op->per_outer_is_easy = op.per_outer_is_easy;
    decide_op->per_inner_was_avg = op.per_inner_was_avg;
    // The flattened constraints and objective, produced by the DECIDE optimizer's
    // final pass. Execution evaluates their coefficients; it does not re-derive them.
    decide_op->prepared = std::move(op.prepared);

    decide_op->keyed_scopes = std::move(op.keyed_scopes);
    decide_op->variable_scopes = std::move(op.variable_scopes);
    // Resolved like every other expression the operator owns (EnumerateExpressions).
    decide_op->keys = std::move(op.keys);
    decide_op->scope_checks = std::move(op.scope_checks);

    // Resolve source column names for the surviving (post-pruning) child columns,
    // positionally aligned with the materialized data chunk. Used by the unbounded
    // diagnosis to label escaping categorical groups (affected_rows).
    //
    // Harvest names from the DECIDE clause's own bound references to source columns
    // (`when` / `per` / `by` / objective / constraint / key columns of keyed decisions). By physical-plan
    // time the column-binding resolver has rewritten table-column references to
    // BoundReferenceExpressions whose `index` is the position in the child data chunk
    // — exactly the indexing of gstate.data — so this aligns directly and survives
    // pruning (decision-variable references stay BoundColumnRef and are skipped).
    // Columns only in the outer SELECT (not the DECIDE clause) get no name from this
    // harvest; they are back-filled below from the child projection's user-written
    // names. Anything still empty after that (a computed column with no user alias) is
    // left blank on purpose: the unbounded diagnosis suppresses categorical rules over
    // unnamed columns rather than inventing a positional `colN` the user never typed.
    decide_op->input_column_names.assign(child_bindings.size(), string());
    decide_op->input_column_in_clause.assign(child_bindings.size(), false);
    // Names are kept as SQL, quoted where they need it, so a remedy that prints one is
    // pasteable. Every reference carries its bare column name (a key column's SQL spelling,
    // `t.depot`, `D."cost"`, is kept beside it in DecideKeyElement::name).
    std::function<void(const Expression &)> harvest;
    // A key names a column as the key was written (`t.depot`), whichever clause added the key first.
    auto harvest_key = [&](idx_t key) {
        for (auto &element : DecideKeyElements(decide_op->keys, key)) {
            auto &expr = *element.expression;
            if (expr.GetExpressionClass() != ExpressionClass::BOUND_REF) {
                harvest(expr);
                continue;
            }
            auto index = expr.Cast<BoundReferenceExpression>().index;
            if (index < decide_op->input_column_names.size()) {
                decide_op->input_column_in_clause[index] = true;
                if (decide_op->input_column_names[index].empty()) {
                    decide_op->input_column_names[index] = element.name;
                }
            }
        }
    };
    harvest = [&](const Expression &e) {
        if (e.GetExpressionClass() == ExpressionClass::BOUND_REF) {
            auto &r = e.Cast<BoundReferenceExpression>();
            if (r.index < decide_op->input_column_names.size()) {
                // Reached from the DECIDE clause, so it explains rather than correlates
                // — recorded even when an earlier reference already supplied the name.
                decide_op->input_column_in_clause[r.index] = true;
                if (decide_op->input_column_names[r.index].empty()) {
                    decide_op->input_column_names[r.index] = QuoteColumnName(StripDecideTags(r.GetName()));
                }
            }
        }
        ExpressionIterator::EnumerateChildren(e, harvest);
        // A `per K:` names its key after its clause, as written.
        if (e.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
            auto &conjunction = e.Cast<BoundConjunctionExpression>();
            if (IsPerClassWrapper(conjunction) || IsTwoLevelObjectiveWrapper(conjunction)) {
                harvest_key(PerWrapperKey(conjunction));
            }
        }
    };
    if (decide_op->decide_constraints) {
        harvest(*decide_op->decide_constraints);
    }
    if (decide_op->decide_objective) {
        harvest(*decide_op->decide_objective);
    }
    // A `by` key and a scope check read the clause's data too; an aggregate-shape clause
    // keeps its `per` wrapper (DecideCanonicalizer::CanonicalizeTreeInternal), so its key
    // is named above. A key only a declaration writes names no column: a column the
    // clauses never name stays unnamed.
    for (idx_t key = 0; key < decide_op->keys.size(); key++) {
        if (decide_op->keys[key].written_as_by) {
            harvest_key(key);
        }
    }
    for (auto &check : decide_op->scope_checks) {
        for (auto *expr : {check.rows.get(), check.value.get(), check.filter.get()}) {
            if (expr) {
                harvest(*expr);
            }
        }
        harvest_key(check.key);
    }
    // A lifted reducer's expressions live beside the tree: a composed term's and a helper
    // decision's (reducer_value_links). The columns they read explain the answer too.
    auto harvest_composed_term = [&](const LogicalDecide::ComposedMinMaxTerm &term) {
        for (auto *expr : {term.inner_expr.get(), term.filter.get(), term.scale.get()}) {
            if (expr) {
                harvest(*expr);
            }
        }
    };
    for (auto &spec : decide_op->composed_minmax_constraints) {
        for (auto &term : spec.terms) {
            harvest_composed_term(term);
        }
        if (spec.rhs_expr) {
            harvest(*spec.rhs_expr);
        }
        if (spec.scope.when) {
            harvest(*spec.scope.when);
        }
        harvest_key(spec.scope.class_key);
    }
    for (auto &term : decide_op->composed_minmax_objective_terms) {
        harvest_composed_term(term);
    }
    for (auto &link : decide_op->reducer_value_links) {
        for (auto *expr : {link.inner_expr.get(), link.filter.get(), link.when.get()}) {
            if (expr) {
                harvest(*expr);
            }
        }
    }
    // Back-fill SELECT-only columns from the child projection's user-written names.
    // Only slots the DECIDE-clause harvest left empty are touched, so a name the user
    // referenced in the clause always wins over the raw projection alias.
    for (idx_t i = 0; i < decide_op->input_column_names.size() && i < child_userwritten_names.size(); i++) {
        if (decide_op->input_column_names[i].empty() && !child_userwritten_names[i].empty()) {
            decide_op->input_column_names[i] = QuoteColumnName(child_userwritten_names[i]);
        }
    }
    return std::move(decide_op);
}

} // namespace duckdb
