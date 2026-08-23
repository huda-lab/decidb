#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"
#include "duckdb/planner/decide/decide_prepared_model.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/decidb/solver_registry.hpp"
#include <unordered_map>

namespace duckdb {

//! Global sink state accumulated by PhysicalDecide::Sink/Combine; defined in
//! physical_decide.cpp. Finalize's private phase methods take it by reference.
class DecideGlobalSinkState;

//! Per-Finalize cache of TransformToChunkExpression results, keyed by the original
//! (unlowered) expression pointer. Outlives every ExpressionExecutor built during
//! Finalize, so it's safe for executors to retain references into cached entries.
using ChunkExprCache = std::unordered_map<const Expression *, unique_ptr<Expression>>;

//! One cached PER grouping: the expression set it was built from, plus the resulting
//! unfiltered row→group assignment. Keyed by a hash of the PER expression set so a
//! PER spec evaluated once (e.g. by a constraint) is reused by every other call site
//! that asks for the same expression set (e.g. the objective, or a later constraint).
struct PerGroupCacheEntry {
    vector<const Expression *> exprs;
    bool null_excludes;
    vector<idx_t> unfiltered_row_group_ids;
    idx_t unfiltered_num_groups;
    //! Representative key values per unfiltered group ([key_col][gid]); used to label
    //! each PER group with its printable key for infeasible diagnosis.
    vector<vector<Value>> unfiltered_rep_keys;
};
using PerGroupCache = std::unordered_map<size_t, vector<PerGroupCacheEntry>>;

//! Per-term filter state for aggregate-local WHEN (constraint terms, bilinear terms,
//! quadratic groups, and their objective-side equivalents all share this shape).
struct TermFilterState {
    vector<bool> mask;
    bool has_filter = false;
    bool avg_scale = false;
};

//===--------------------------------------------------------------------===//
// PhysicalDecide Operator
//===--------------------------------------------------------------------===//

//! PhysicalDecide represents a blocking operator that solves an ILP over its
//! entire input before producing output.
class PhysicalDecide : public PhysicalOperator {
public:
    static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::DECIDE;

public:
    PhysicalDecide(vector<LogicalType> types, idx_t estimated_cardinality, 
                    unique_ptr<PhysicalOperator> child, idx_t decide_index, 
                    vector<unique_ptr<Expression>> decide_variables,
                    unique_ptr<Expression> decide_constraints, DecideSense decide_sense,
                    unique_ptr<Expression> decide_objective);

    // The table index for the new columns
    idx_t decide_index;

    // The variables to be decided (e.g., x, y)
    vector<unique_ptr<Expression>> decide_variables;

    // O(1) lookup: ColumnBinding → decide_variables index (built in constructor)
    column_binding_map_t<idx_t> decide_variable_map;

    // The bound constraints expression
    unique_ptr<Expression> decide_constraints;

    //! Stable source display registry copied from LogicalDecide.
    vector<ConstraintSourceInfo> constraint_sources;

    //! The user's written spelling of every cast and scalar subquery in the DECIDE
    //! clause, copied from LogicalDecide. RenderDecideSource replays it so EXPLAIN
    //! and the diagnosis labels quote the clause the user typed.
    vector<string> source_fragments;

    // The optimization sense (MINIMIZE or MAXIMIZE)
    DecideSense decide_sense;

    // The bound objective function expression
    unique_ptr<Expression> decide_objective;

    // Number of auxiliary variables (e.g. from ABS linearization) at the end of decide_variables
    idx_t num_auxiliary_vars = 0;

    // Per-variable flag: true if the variable was declared IS BOOLEAN (copied from
    // LogicalDecide). A BOOLEAN variable is lowered to an INTEGER with a 0/1 domain
    // (synthesized `x >= 0` / `x <= 1` constraints), so its runtime type is INTEGER;
    // this flag is the only surviving signal that the 0/1 box is the variable's
    // intrinsic domain, not a user-editable bound. Indexed by decide_variables position.
    vector<bool> is_boolean_var;

    // Indices of auxiliary indicator variables for not-equal (<>) constraints
    vector<idx_t> ne_indicator_indices;

    // Links from MIN/MAX indicator variables: (agg_name "min"/"max", indicator_idx)
    vector<string> minmax_clause_labels;

    // Links from bilinear McCormick auxiliary variables: w = b * x
    // (aux_idx, bool_var_idx, other_var_idx) — for execution-time Big-M constraint generation
    vector<LogicalDecide::BilinearLink> bilinear_links;
    vector<LogicalDecide::AbsMaximizeLink> abs_maximize_links;

    // F6: auxiliary variable index -> user's original source expression string
    // (ABS(...) / MIN/MAX(...) / product / <>), for naming an escaping aux column
    // in the unbounded diagnosis. Sparse; only auxiliary variables appear.
    vector<pair<idx_t, string>> aux_var_expressions;

    // Composed MIN/MAX constraints (additive LHS with MIN/MAX terms mixed in).
    // Each is emitted as a block of RawConstraints in global_constraints at sink finalize.
    vector<LogicalDecide::ComposedMinMaxConstraint> composed_minmax_constraints;

    // Composed MIN/MAX objective: additive sum of SUM/AVG/MIN/MAX terms.
    // Empty when the objective is not composed.
    vector<LogicalDecide::ComposedMinMaxTerm> composed_minmax_objective_terms;

    // --- Absorbed variable bounds (decided by DecideOptimizer::AbsorbVariableBounds) ---
    //
    // The decision column box, already resolved upstream. Finalize copies these into
    // SolverInput; comparisons folded into them carry ABSORBED_BOUND_TAG so term
    // extraction skips them. Execution consumes this, it does not re-derive it.
    vector<double> absorbed_lower_bounds;
    vector<double> absorbed_upper_bounds;
    // Every absorbed user bound, kept so the infeasible diagnosis can re-emit it as a
    // slackable row and quote it as written. A BETWEEN contributes two entries.
    vector<LogicalDecide::UserBoundSpec> user_absorbed_bounds;

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

    //! Entity scope info for each source table with table-scoped variables
    vector<EntityScopeInfo> entity_scopes;

    //! Per-variable scope assignment (row / entity / query-wide scalar);
    //! entity_scope_idx indexes entity_scopes
    vector<DecideVarScopeInfo> variable_scopes;

    //! Source column name per physical child-output column (positionally aligned
    //! with gstate.data columns), resolved post-pruning in plan_decide.cpp. Used by
    //! the unbounded diagnosis to label escaping categorical groups (affected_rows).
    vector<string> input_column_names;

    //! The registered NAME of the backend this query was planned for, chosen once by
    //! stage 05 (LogicalDecide::solver_backend_name) and copied here at physical
    //! planning. Layer 8 READS it — the primary solve and every diagnostic re-solve run
    //! on this one backend — and never re-derives it: the rewrites upstream already
    //! committed to what this backend takes natively, so a second, differently-answered
    //! selection would run a model on a solver it was not built for. It is resolved back
    //! to a backend only where a solve is about to happen (SolverRegistry::Find).
    string solver_backend_name;

    //! Stage 05's formulation decision, carried down verbatim: which constructs this
    //! query hands to the backend to state itself, and which arrive already lowered into
    //! plain rows. Layer 8 READS this and translates. It must NOT ask a backend what it
    //! supports — choosing a formulation is stage 05's job, and asking again here is how
    //! the plan and the solve come to disagree about what was lowered.
    SolverConstructSupport use_native_constructs;

    //! Stage 05's formulation POLICY, beside the capability table above: whether a
    //! declared construct is used wherever it is declared, or only where the lowering
    //! has no valid Big-M. False is the shipping answer — the lowering is the smaller
    //! model wherever it works, so native is the fallback (see NativeConstructPolicy).
    //! True only under the test-only `DECIDB_NATIVE_CONSTRUCTS=force`.
    //!
    //! Whether a given SITE has a valid Big-M depends on evaluated data, so stage 08
    //! answers that question. It does not re-decide the policy: it applies this one to
    //! a fact only it can see, the same way the `<>` range collapse does.
    bool force_native_constructs = false;

    // --- Prepared linear form (decided by BuildDecidePreparedModel, stage 05) ---

    //! Every constraint and the objective, already flattened into additive terms
    //! whose coefficients are still unevaluated expressions. PHASE 2 evaluates those
    //! coefficients against the materialized rows; nothing here re-derives the shape.
    DecidePreparedModel prepared;

public:
    // Source interface
    unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
    SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;

    // Sink interface
    unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
    unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
    SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
    SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
    SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                              OperatorSinkFinalizeInput &input) const override;

    bool IsSource() const override {
        return true;
    }

    bool IsSink() const override {
        return true;
    }
    bool ParallelSink() const override {
        return true;
    }

public:
    string GetName() const override;
    InsertionOrderPreservingMap<string> ParamsToString() const override;

private:
    //! The backend named on the plan, resolved to a registry entry. This is the ONLY
    //! place layer 8 turns `solver_backend_name` back into a backend, and it happens
    //! where a solve is about to run — the primary one and every diagnostic re-solve.
    //! Layer 8 never re-selects: it looks up the name stage 05 recorded.
    SolverBackend PlannedSolverBackend() const;

    //! PHASE 1.5: one row→entity mapping per table-scoped entity scope.
    vector<EntityMapping> BuildEntityMappings(ClientContext &context, DecideGlobalSinkState &gstate,
                                              idx_t num_rows) const;

    //! PHASE 2, sub-phase 1: evaluate every constraint's coefficients and append the
    //! result to gstate.evaluated_constraints.
    void EvaluateConstraints(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                             ChunkExprCache &chunk_expr_cache, PerGroupCache &per_group_cache,
                             const vector<EntityMapping> &entity_mappings) const;

    //! Per-term filter state the objective evaluation produces and Phase 3 (qualifier
    //! de-dup, AVG scaling, MIN/MAX objective linearization) still needs.
    struct ObjectiveEvalState {
        vector<TermFilterState> linear_filters, quadratic_filters, bilinear_filters;
        vector<bool> when_mask;
        bool has_when = false;
    };

    //! PHASE 2, sub-phase 2: evaluate the objective's coefficients onto gstate's
    //! objective-evaluation fields.
    ObjectiveEvalState EvaluateObjective(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                                         ChunkExprCache &chunk_expr_cache, PerGroupCache &per_group_cache,
                                         const vector<EntityMapping> &entity_mappings) const;

    //! PHASE 3, model build: SolverInput assembly, auto-M, linearization passes,
    //! objective transfer, objective PER-grouping + AVG scaling, VarIndexer::Build,
    //! MIN/MAX objective linearization, and the composed-MIN/MAX block. Everything up
    //! to (not including) the SolveModel call.
    SolverInput BuildSolverInput(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                                 ChunkExprCache &chunk_expr_cache, PerGroupCache &per_group_cache,
                                 vector<EntityMapping> entity_mappings, ObjectiveEvalState obj_state,
                                 VarIndexer &out_var_indexer, bool bench, Profiler &model_timer) const;

    //! PHASE 3, solve + readback: build_var_labels, the RouteSolveResult switch and its
    //! four terminal cases, and the shared success epilogue.
    SinkFinalizeType FinalizeSolveResult(ClientContext &context, DecideGlobalSinkState &gstate,
                                         SolverInput &solver_input, VarIndexer &var_indexer, bool bench,
                                         Profiler &model_timer, Profiler &solver_timer) const;
};

} // namespace duckdb
