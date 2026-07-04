#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <functional>

namespace duckdb {

unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreatePlan(LogicalDecide &op) {
    D_ASSERT(op.children.size() == 1);
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
    decide_op->num_auxiliary_vars = op.num_auxiliary_vars;
    decide_op->is_boolean_var = op.is_boolean_var;
    decide_op->ne_indicator_indices = std::move(op.ne_indicator_indices);
    decide_op->minmax_indicator_links = std::move(op.minmax_indicator_links);
    decide_op->bilinear_links = std::move(op.bilinear_links);
    decide_op->abs_maximize_links = std::move(op.abs_maximize_links);
    decide_op->aux_var_expressions = std::move(op.aux_var_expressions);
    decide_op->composed_minmax_constraints = std::move(op.composed_minmax_constraints);
    decide_op->composed_minmax_objective_terms = std::move(op.composed_minmax_objective_terms);
    decide_op->flat_objective_agg = op.flat_objective_agg;
    decide_op->flat_objective_is_easy = op.flat_objective_is_easy;
    decide_op->per_inner_agg = op.per_inner_agg;
    decide_op->per_outer_agg = op.per_outer_agg;
    decide_op->per_inner_is_easy = op.per_inner_is_easy;
    decide_op->per_outer_is_easy = op.per_outer_is_easy;
    decide_op->per_inner_was_avg = op.per_inner_was_avg;

    // Resolve entity key column bindings to physical data chunk positions.
    // The child's GetColumnBindings() gives us the mapping from logical
    // (table_index, col_index) to physical chunk position.
    // Only include columns that survived DuckDB's column pruning.
    if (!op.entity_scopes.empty()) {
        // Refresh entity_key_bindings from entity_key_expressions: the column
        // pruner rebinds the expressions alongside other columns, but the stale
        // bindings on EntityScopeInfo are not updated.
        idx_t expr_cursor = 0;
        for (auto &scope : op.entity_scopes) {
            for (idx_t k = 0; k < scope.entity_key_bindings.size(); k++) {
                if (expr_cursor < op.entity_key_expressions.size()) {
                    auto &colref = op.entity_key_expressions[expr_cursor]->Cast<BoundColumnRefExpression>();
                    scope.entity_key_bindings[k] = colref.binding;
                    expr_cursor++;
                }
            }
        }
        for (auto &scope : op.entity_scopes) {
            scope.entity_key_physical_indices.clear();
            vector<LogicalType> surviving_types;
            for (idx_t k = 0; k < scope.entity_key_bindings.size(); k++) {
                auto &target = scope.entity_key_bindings[k];
                for (idx_t pos = 0; pos < child_bindings.size(); pos++) {
                    if (child_bindings[pos].table_index == target.table_index &&
                        child_bindings[pos].column_index == target.column_index) {
                        scope.entity_key_physical_indices.push_back(pos);
                        surviving_types.push_back(scope.entity_key_column_types[k]);
                        break;
                    }
                }
            }
            scope.entity_key_column_types = std::move(surviving_types);
        }
    }
    decide_op->entity_scopes = std::move(op.entity_scopes);
    decide_op->variable_entity_scope = std::move(op.variable_entity_scope);

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
    for (auto &e : op.entity_key_expressions) {
        if (e) {
            harvest(*e);
        }
    }
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