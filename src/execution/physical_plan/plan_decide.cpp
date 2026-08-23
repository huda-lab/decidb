#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/decidb/ilp_solver.hpp"
#include "duckdb/optimizer/decide_linear_form.hpp"
#include "duckdb/optimizer/decide_solver_gate.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <functional>

namespace duckdb {

unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreatePlan(LogicalDecide &op) {
    D_ASSERT(op.children.size() == 1);
    // The backend — and with it stage 05's decision about which constructs stay native —
    // was settled before any rewrite ran, so the rewrites and the solve agree on what is
    // lowered. If the DECIDE optimizer never ran (`SET
    // disabled_optimizers='decide_optimizer'`), nothing settled it; trigger the
    // stage-05-owned pass here so the operator still runs against exactly one backend
    // and one formulation for the whole query. It is a no-op when a choice is already on
    // the plan, so it can never overwrite one the rewrites were selected against.
    ChooseDecideSolver(op);
    // Both clauses are verified after ALL optimizer rewriting and before any logical
    // expression is moved into the physical operator. The objective is checked here
    // for the same reason the constraints are: every rewrite that touches it now
    // re-enters through LogicalDecide::SetObjective, so a rewrite that breaks the
    // canonical form should fail loudly rather than reach the extractor.
    if (op.decide_constraints || op.decide_objective) {
        DecideCanonicalizer canonicalizer(context, op.decide_index, op.variable_scopes);
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
    // column_ids, which is why base-table entity scopes worked and subquery /
    // CTE-backed scopes silently collapsed to a single entity.
    auto child_bindings = op.children[0]->GetColumnBindings();
    // Capture user-written column names off the child projection BEFORE CreatePlan
    // moves its `expressions` vector out (same reason as child_bindings above). These
    // back-fill names for columns that appear ONLY in the outer SELECT — never in the
    // DECIDE clause — so the unbounded diagnosis can characterize an escaping slice by
    // them. We keep only names the user actually wrote: an explicit `AS name` alias, or
    // a bare source-column reference. A computed projection expression with no alias
    // carries only a generated name (its ToString), which we deliberately drop so the
    // diagnosis never prints a label the user never typed.
    vector<string> child_userwritten_names(child_bindings.size(), string());
    if (op.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION) {
        auto &child_proj = op.children[0]->Cast<LogicalProjection>();
        for (idx_t i = 0; i < child_proj.expressions.size() && i < child_userwritten_names.size();
             i++) {
            auto &expr = *child_proj.expressions[i];
            if (expr.HasAlias()) {
                child_userwritten_names[i] = expr.GetAlias();
            } else if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                child_userwritten_names[i] = expr.Cast<BoundColumnRefExpression>().GetName();
            }
        }
    }
    auto child_plan = CreatePlan(*op.children[0]);
    auto decide_op = make_uniq<PhysicalDecide>(
        op.types, op.estimated_cardinality, std::move(child_plan),
        op.decide_index, std::move(op.decide_variables),
        std::move(op.decide_constraints), op.decide_sense, std::move(op.decide_objective));
    decide_op->solver_backend_name = op.solver_backend_name;
    decide_op->use_native_constructs = op.use_native_constructs;
    decide_op->force_native_constructs = op.force_native_constructs;
    decide_op->num_auxiliary_vars = op.num_auxiliary_vars;
    decide_op->is_boolean_var = op.is_boolean_var;
    decide_op->ne_clause_labels = std::move(op.ne_clause_labels);
    decide_op->minmax_clause_labels = std::move(op.minmax_clause_labels);
    decide_op->bilinear_links = std::move(op.bilinear_links);
    decide_op->abs_maximize_links = std::move(op.abs_maximize_links);
    decide_op->aux_var_expressions = std::move(op.aux_var_expressions);
    decide_op->constraint_sources = std::move(op.constraint_sources);
    decide_op->source_fragments = std::move(op.source_fragments);
    decide_op->composed_minmax_constraints = std::move(op.composed_minmax_constraints);
    decide_op->composed_minmax_objective_terms = std::move(op.composed_minmax_objective_terms);
    decide_op->absorbed_lower_bounds = std::move(op.absorbed_lower_bounds);
    decide_op->absorbed_upper_bounds = std::move(op.absorbed_upper_bounds);
    decide_op->user_absorbed_bounds = std::move(op.user_absorbed_bounds);
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

    // Entity key expressions are resolved by the column-binding resolver like every
    // other expression LogicalDecide owns (LogicalDecide::EnumerateExpressions), so by
    // this point each one holds a BoundReferenceExpression naming its physical position
    // in the child data chunk directly -- no separate lookup against child_bindings is
    // needed.
    if (!op.entity_scopes.empty()) {
        idx_t expr_cursor = 0;
        for (auto &scope : op.entity_scopes) {
            scope.entity_key_physical_indices.clear();
            for (idx_t k = 0; k < scope.entity_key_bindings.size(); k++) {
                D_ASSERT(expr_cursor < op.entity_key_expressions.size());
                auto &ref = op.entity_key_expressions[expr_cursor]->Cast<BoundReferenceExpression>();
                scope.entity_key_physical_indices.push_back(ref.index);
                expr_cursor++;
            }
        }
    }
    decide_op->entity_scopes = std::move(op.entity_scopes);
    decide_op->variable_scopes = std::move(op.variable_scopes);

    // Resolve source column names for the surviving (post-pruning) child columns,
    // positionally aligned with the materialized data chunk. Used by the unbounded
    // diagnosis to label escaping categorical groups (affected_rows).
    //
    // Harvest names from the DECIDE clause's own bound references to source columns
    // (WHEN / PER / objective / constraint / entity-key columns). By physical-plan
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
    std::function<void(const Expression &)> harvest = [&](const Expression &e) {
        if (e.GetExpressionClass() == ExpressionClass::BOUND_REF) {
            auto &r = e.Cast<BoundReferenceExpression>();
            if (r.index < decide_op->input_column_names.size() &&
                decide_op->input_column_names[r.index].empty()) {
                decide_op->input_column_names[r.index] = r.GetName();
            }
        }
        ExpressionIterator::EnumerateChildren(e, harvest);
    };
    if (decide_op->decide_constraints) {
        harvest(*decide_op->decide_constraints);
    }
    if (decide_op->decide_objective) {
        harvest(*decide_op->decide_objective);
    }
    // entity_key_expressions are deliberately NOT harvested for names: every entry
    // carries the synthetic "entity_key_<alias>" placeholder (see
    // LogicalDecide::entity_key_expressions / bind_select_node.cpp), needed only to
    // keep the pruner and column-binding resolver treating the column as live, never
    // meant as a display name. A real name still reaches a given entity-key column
    // through decide_constraints/decide_objective (if referenced there) or the
    // back-fill below (if selected in the outer query); columns named only this way
    // stay unnamed on purpose, same as any other column no clause referenced by name.
    // Back-fill SELECT-only columns from the child projection's user-written names.
    // Only slots the DECIDE-clause harvest left empty are touched, so a name the user
    // referenced in the clause always wins over the raw projection alias.
    for (idx_t i = 0; i < decide_op->input_column_names.size() && i < child_userwritten_names.size();
         i++) {
        if (decide_op->input_column_names[i].empty()) {
            decide_op->input_column_names[i] = child_userwritten_names[i];
        }
    }
    return std::move(decide_op);
}

} // namespace duckdb
