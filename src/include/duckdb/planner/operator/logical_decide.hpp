//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/operator/logical_decide.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/common/enums/decide.hpp"

namespace duckdb {

//! Tracks entity-scope metadata for decision variables scoped to a base table.
//! When a variable is declared as "T.x IS BOOLEAN", it has one value per unique
//! row in table T, not per join result row.
struct EntityScopeInfo {
    //! Table alias or name used in the DECIDE declaration (e.g., "S" or "Sensors")
    string table_alias;
    //! DuckDB table index from the bind context (Binding::index)
    idx_t source_table_index;
    //! Column types for the entity key columns
    vector<LogicalType> entity_key_column_types;
    //! Physical column indices in the child's output data chunk.
    //! These are resolved during physical plan creation (plan_decide.cpp)
    //! from the logical column bindings by matching against the child's GetColumnBindings().
    vector<idx_t> entity_key_physical_indices;
    //! Logical column bindings (table_index, col_index) — used to resolve physical indices
    vector<ColumnBinding> entity_key_bindings;
    //! Which decide_variables indices are scoped to this table
    vector<idx_t> scoped_variable_indices;
};

class LogicalDecide : public LogicalOperator {
public:
    static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_DECIDE;

public:
    LogicalDecide(idx_t decide_index, vector<unique_ptr<Expression>> decide_variables,
                  unique_ptr<Expression> decide_constraints, DecideSense decide_sense,
                  unique_ptr<Expression> decide_objective);

    LogicalDecide();

    // The table index for the new columns
    idx_t decide_index;

    // The variables to be decided (e.g., x, y)
    vector<unique_ptr<Expression>> decide_variables;

    // The bound constraints expression
    unique_ptr<Expression> decide_constraints;

    // The optimization sense (MINIMIZE or MAXIMIZE)
    DecideSense decide_sense;

    // The bound objective function expression
    unique_ptr<Expression> decide_objective;

    // Additive constant peeled from the parsed objective body during
    // NormalizeDecideObjective (e.g. the `3` in `MAXIMIZE SUM(x) + 3`).
    // The solver ignores this — it doesn't change argmax/argmin — but it's
    // preserved here so a future "report the objective value" feature can
    // add it back without losing information. Zero when nothing was peeled.
    double objective_constant_offset = 0.0;

    // Number of auxiliary variables at the end of decide_variables (created by binder and optimizer)
    idx_t num_auxiliary_vars = 0;

    // Per-variable boolean flag: true if the variable was declared IS BOOLEAN.
    // Indexed by position in decide_variables. Auxiliary variables appended later
    // should also push_back their boolean status.
    vector<bool> is_boolean_var;

    // Indices of auxiliary indicator variables for not-equal (<>) constraints
    vector<idx_t> ne_indicator_indices;

    // Links from MIN/MAX indicator variables: (agg_name "min"/"max", indicator_idx)
    vector<pair<string, idx_t>> minmax_indicator_links;

    // Links from bilinear McCormick auxiliary variables: w = b * x
    // (aux_idx, bool_var_idx, other_var_idx) — for execution-time Big-M constraint generation
    struct BilinearLink {
        idx_t aux_idx;        // Index of auxiliary variable w
        idx_t bool_var_idx;   // Index of the Boolean variable b
        idx_t other_var_idx;  // Index of the non-Boolean variable x
    };
    vector<BilinearLink> bilinear_links;

    // Links for MAXIMIZE+ABS Big-M upper-bound constraints.
    // Produced by RewriteAbs when sense==MAXIMIZE and ABS is in the objective.
    // At execution time, the two lower-bound EvaluatedConstraints tagged with
    // ABS_UB_POS_TAG_PREFIX / ABS_UB_NEG_TAG_PREFIX are used to derive and emit
    // two upper-bound constraints that pin aux = |inner| exactly.
    struct AbsMaximizeLink {
        idx_t aux_idx;   // ABS auxiliary variable
        idx_t y_idx;     // binary sign indicator variable
    };
    vector<AbsMaximizeLink> abs_maximize_links;

    //! Maps an auxiliary variable's index in decide_variables to a human-readable
    //! source expression (the user's original ABS(...) / MAX(...) / product / <>),
    //! captured at optimizer time for diagnosis variable-naming (F6 variable
    //! provenance). Sparse — only auxiliary variables appear. Consumed by the
    //! unbounded diagnosis to name an escaping aux column by its source expression
    //! rather than its internal __abs_aux_N__ name.
    vector<pair<idx_t, string>> aux_var_expressions;

    //! Composed MIN/MAX constraint: additive LHS with one or more MIN/MAX terms alongside
    //! SUM/AVG terms. Each term becomes a global auxiliary at execution time (MIN/MAX) or
    //! is summed into the outer ILP row (SUM/AVG). See DecideOptimizer::RewriteComposedMinMax.
    struct ComposedMinMaxTerm {
        enum Kind { SUM_KIND, MINMAX_KIND };
        Kind kind;
        string agg_name;                      // "sum", "avg", "min", "max"
        int sign;                             // +1 or -1 (from subtraction)
        unique_ptr<Expression> inner_expr;    // The expression inside the aggregate
        unique_ptr<Expression> filter;        // Aggregate-local WHEN filter (optional)
        bool is_easy = true;                  // For MIN/MAX: easy (no Big-M) or hard (indicator).
    };
    struct ComposedMinMaxConstraint {
        vector<ComposedMinMaxTerm> terms;
        unique_ptr<Expression> rhs_expr;      // RHS expression (typically scalar constant)
        ExpressionType outer_cmp;             // Outer comparison (<=, >=, <, >)
    };
    vector<ComposedMinMaxConstraint> composed_minmax_constraints;

    //! Composed MIN/MAX objective: additive sum of SUM/AVG/MIN/MAX terms in the objective.
    //! Populated by DecideOptimizer::RewriteComposedMinMaxObjective. Empty when the
    //! objective is a single aggregate (handled by RewriteMinMaxObjective) or plain linear.
    vector<ComposedMinMaxTerm> composed_minmax_objective_terms;

    // --- MIN/MAX objective metadata (set by DecideOptimizer::RewriteMinMaxObjective) ---

    // Flat (non-PER) objective: original aggregate type before rewrite to SUM
    ObjectiveAggregateType flat_objective_agg = ObjectiveAggregateType::NONE;
    // Pre-computed: true if easy formulation (MAXIMIZE+MIN or MINIMIZE+MAX)
    bool flat_objective_is_easy = false;

    // PER nested objective: OUTER(INNER(expr)) PER col
    ObjectiveAggregateType per_inner_agg = ObjectiveAggregateType::NONE;
    ObjectiveAggregateType per_outer_agg = ObjectiveAggregateType::NONE;
    // Pre-computed easy/hard at each level (only meaningful when agg is MIN_AGG or MAX_AGG)
    bool per_inner_is_easy = false;
    bool per_outer_is_easy = false;
    // True if inner aggregate was originally AVG (coefficients need 1/n_g scaling)
    bool per_inner_was_avg = false;

    // --- Table-scoped variable metadata ---

    //! Entity scope info for each source table with table-scoped variables.
    //! Empty if all variables are row-scoped (default behavior).
    vector<EntityScopeInfo> entity_scopes;

    //! Per-variable scope assignment: INVALID_INDEX = row-scoped (default),
    //! otherwise index into entity_scopes.
    vector<idx_t> variable_entity_scope;

    //! BoundColumnRefExpressions for every entity-key column (flattened in scope order).
    //! These live here so that DuckDB's binder initial column_id selection AND the
    //! RemoveUnusedColumns pruner track them as live references. Without them,
    //! entity-key columns that aren't referenced elsewhere (SELECT/WHERE/
    //! constraints/objective) would be pruned from the table scan, silently
    //! collapsing distinct entities into whatever grouping happens to survive.
    //! plan_decide.cpp reads refreshed bindings from these expressions (kept in
    //! sync by the pruner's rebinding pass) and copies them back into
    //! entity_scopes.entity_key_bindings.
    vector<unique_ptr<Expression>> entity_key_expressions;

public:
    // --- Implement virtual functions ---

    // The output columns are the child's columns plus the new decide variables
    vector<ColumnBinding> GetColumnBindings() override;

    // Resolve the output types
    void ResolveTypes() override;

    string GetName() const override;
    InsertionOrderPreservingMap<string> ParamsToString() const override;

    void Serialize(Serializer &serializer) const override;
    static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);
    
protected:
    // The table indices that this operator produces
    vector<idx_t> GetTableIndex() const override;

private:
    //! Recursively render a tagged expression (constraint AND-tree or objective),
    //! unwrapping WHEN/PER wrappers into postfix suffixes for display. Used for
    //! both the Constraints and Objective EXPLAIN rows so they render symmetrically.
    static void CollectTaggedExpressionStrings(const Expression &expr, vector<string> &out);
};

} // namespace duckdb