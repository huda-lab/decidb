#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/decide/decide_prepared_model.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/decidb/solver/solver_registry.hpp"
#include <unordered_map>

namespace duckdb {

//! Global sink state accumulated by PhysicalDecide::Sink/Combine; defined in
//! physical_decide.cpp. Finalize's private phase methods take it by reference.
class DecideGlobalSinkState;

//! Per-Finalize cache of TransformToChunkExpression results, keyed by the original
//! (unlowered) expression pointer. Outlives every ExpressionExecutor built during
//! Finalize, so it's safe for executors to retain references into cached entries.
using ChunkExprCache = std::unordered_map<const Expression *, unique_ptr<Expression>>;

//! One cached key grouping: the expression set it was built from, plus the resulting
//! unfiltered row→group assignment. Keyed by a hash of the expression set so a key
//! evaluated once (e.g. by a constraint) is reused by every other call site that asks
//! for the same expression set (e.g. the objective, or a later constraint).
struct PerGroupCacheEntry {
    vector<const Expression *> exprs;
    vector<idx_t> unfiltered_row_group_ids;
    idx_t unfiltered_num_groups;
    //! Representative key values per unfiltered group ([key_col][gid]); used to label
    //! each group with its printable key for infeasible diagnosis.
    vector<vector<Value>> unfiltered_rep_keys;
};
using PerGroupCache = std::unordered_map<size_t, vector<PerGroupCacheEntry>>;

//! Per-term filter state for a reducer's own `when` (constraint terms, bilinear terms,
//! quadratic groups, and their objective-side equivalents all share this shape).
struct TermFilterState {
    vector<bool> mask;
    bool has_filter = false;
    bool avg_scale = false;
    //! An objective AVG's `by` groups among the rows it reads (DecideTerm::avg_group_key),
    //! which its 1/n divides by; empty for one group.
    vector<idx_t> avg_group_ids;
    idx_t avg_num_groups = 0;
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

    //! The objective as written and as canonicalized, copied from LogicalDecide so the
    //! physical plan renders the same layers the logical one does.
    string written_objective;
    string canonical_objective;

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
    vector<string> ne_clause_labels;

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

    //! The helper decisions of the value rewrite and the reducers they hold
    //! (LogicalDecide::reducer_value_links); PHASE 2 evaluates each one's members.
    vector<LogicalDecide::ReducerValueLink> reducer_value_links;
    //! What each rewrite made of a clause, for EXPLAIN (LogicalDecide::rewrite_accounts).
    vector<LogicalDecide::RewriteAccount> rewrite_accounts;
    //! What the data made of a scoped expression whose classes held more than one variant
    //! (Kind::VARIANTS, D64): written by Finalize, read by ParamsToString, which the
    //! profiler calls again once the result is read -- so EXPLAIN ANALYZE prints it, and
    //! a plain EXPLAIN, which runs nothing, does not. Run-time facts of the one execution,
    //! hence mutable.
    mutable vector<LogicalDecide::RewriteAccount> variant_accounts;

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

    // A MIN/MAX objective: original aggregate type before rewrite to SUM
    ObjectiveAggregateType flat_objective_agg = ObjectiveAggregateType::NONE;
    // Pre-computed: true if easy formulation (MAXIMIZE+MIN or MINIMIZE+MAX)
    bool flat_objective_is_easy = false;

    // --- Key scopes ---

    //! Every key scope: of a `per K:` decision, a reducer's `per K:` or colon-form
    //! qualifier, and the value rewrite's helper decisions
    vector<EntityScopeInfo> entity_scopes;

    //! Per-variable scope assignment (row / keyed / query-wide);
    //! entity_scope_idx indexes entity_scopes
    vector<DecideVarScopeInfo> variable_scopes;

    //! Every `by (key)` of the clause, its expressions resolved against the input chunk.
    vector<DecideGroupKey> group_keys;

    //! The frame selectors (LogicalDecide::frames), their expressions resolved against the
    //! input chunk.
    vector<DecideFrameSpec> frames;

    //! The run-time checks of the clauses, run first.
    vector<DecideScopeCheck> scope_checks;
    //! The variant signatures (LogicalDecide::signatures), their expressions resolved
    //! against the input chunk, and the objective's, INVALID_INDEX for none.
    vector<DecideSignature> signatures;
    idx_t objective_signature = DConstants::INVALID_INDEX;

    //! Source column name per physical child-output column (positionally aligned
    //! with gstate.data columns), resolved post-pruning in plan_decide.cpp. Used by
    //! the unbounded diagnosis to label escaping categorical groups (affected_rows).
    vector<string> input_column_names;

    //! Positionally aligned with `input_column_names`: true when the DECIDE clause
    //! itself references the column (a `when`, a key, a constraint, the objective), false when
    //! it only rides along in the outer SELECT. The unbounded characterization uses it
    //! to pick a representative when several columns describe the same escaping rows:
    //! a column the user put in the clause explains the escape, one that merely
    //! correlates with it is a coincidence.
    vector<bool> input_column_in_clause;

    //! The `DIAGNOSE` prefix the user wrote, carried down from LogicalDecide::diagnose.
    //! It is the ONLY thing that arms the diagnosis engines. When true a failed solve
    //! stashes its findings for the DIAGNOSE operator above to return as rows instead of
    //! raising; when false — every unprefixed query — a failed solve reports its status
    //! and stops, and none of the diagnostic prep (unbounded-ray extraction, model
    //! retention, the elastic re-solves) is done or paid for.
    bool diagnose = false;

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

    //! PHASE 1.4: each frame selector's timelines and selections (gstate.frame_maps), then
    //! the values the frames read appended to the input as columns
    //! (DecidePreparedModel::frame_columns), so every later phase reads them as data.
    void BuildFrames(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                     ChunkExprCache &chunk_expr_cache) const;
    //! D67, after the scope checks: the rows of a selected position read one payload
    //! (CheckDecideFramePeers).
    void CheckFramePeers(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                         ChunkExprCache &chunk_expr_cache, const vector<EntityMapping> &entity_mappings) const;
    //! The rows at which every frame the comparison `source_clause_id` reads outside its
    //! reducers selects a position (DecideFrameSpec::SkipsInstancesOf); empty when none may
    //! miss one. A row where one does not is no instance (syntax_reference.md §5.6).
    vector<bool> FramesPresent(const DecideGlobalSinkState &gstate, idx_t source_clause_id, idx_t num_rows) const;
    //! The rows at which `constraint` states an instance: where every frame its clause reads
    //! outside its reducers selects (FramesPresent) and every helper decision it reads there
    //! is present (ReducerValueLink::SkipsInstancesOf), each clause's helper mask computed
    //! once into `helper_masks`. Empty when no instance is skipped; always empty for a row
    //! defining a reducer's members (DecideConstraint::defines_members), which serves every
    //! instance.
    vector<bool> InstancesPresent(const DecideGlobalSinkState &gstate, const DecideConstraint &constraint,
                                  const vector<EntityMapping> &entity_mappings,
                                  unordered_map<idx_t, vector<bool>> &helper_masks, idx_t num_rows) const;
    //! Whether the instance of helper `link` (into reducer_value_links) read at `row` is
    //! present (DecideGlobalSinkState::helper_present).
    bool HelperPresentAt(const DecideGlobalSinkState &gstate, const vector<EntityMapping> &entity_mappings,
                         idx_t link, idx_t row) const;

    //! PHASE 1.5: one row→entity mapping per key scope -- a keyed decision's key columns,
    //! or a helper decision's `by` key over the rows its clause keeps
    //! (EntityScopeInfo::value_link).
    vector<EntityMapping> BuildEntityMappings(ClientContext &context, DecideGlobalSinkState &gstate,
                                              idx_t num_rows, ChunkExprCache &chunk_expr_cache) const;

    //! PHASE 1.6: the scope checks (syntax_reference.md §7.3), before any clause is
    //! evaluated, so every later phase may assume a well-defined clause. Raises the first
    //! one the data fails: a `when` that keeps no row, a reducer with no row in some `by`
    //! group, an objective that would be produced more than once (D66).
    void RunScopeChecks(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                        ChunkExprCache &chunk_expr_cache, PerGroupCache &per_group_cache,
                        const vector<EntityMapping> &entity_mappings) const;
    //! What tells the first two variants of `ids` (a signature's numbering) apart, for a
    //! message: the first field whose value, or instance, differs between them.
    string DescribeVariantDifference(const DecideSignature &signature, const vector<idx_t> &ids,
                                     ChunkExprCache &chunk_expr_cache, ClientContext &context,
                                     DecideGlobalSinkState &gstate, idx_t num_rows, const vector<bool> &rows,
                                     const vector<EntityMapping> &entity_mappings) const;
    //! Records an EXPLAIN ANALYZE line (variant_accounts) for a scoped expression that
    //! generated `variants` instances -- `unit`: constraints, or a reducer's values -- from
    //! `classes` classes.
    void RecordVariants(const DecideSignature &signature, const char *unit, idx_t variants, idx_t classes) const;
    //! Each variant's data field values, for a diagnosis label; `first_rows` holds each
    //! variant's id on its first row only.
    vector<string> DescribeVariants(const DecideSignature &signature, const vector<idx_t> &first_rows,
                                    ChunkExprCache &chunk_expr_cache, ClientContext &context,
                                    DecideGlobalSinkState &gstate, idx_t num_rows) const;

    //! PHASE 2, sub-phase 1: evaluate every constraint's coefficients and append the
    //! result to gstate.evaluated_constraints.
    void EvaluateConstraints(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                             ChunkExprCache &chunk_expr_cache, PerGroupCache &per_group_cache,
                             const vector<EntityMapping> &entity_mappings) const;

    //! Everything PHASE 2 reads off the data that a later phase still needs. It carries
    //! objective terms *and* composed-MIN/MAX constraint clauses, so it is named for the
    //! phase that produces it rather than for one of its consumers. Nothing here depends
    //! on a variable bound, which is what lets the formulation pass run twice against
    //! two different boxes from the same evaluated clauses.
    struct EvaluatedClauses {
        //! Per-term filter state for the objective's linear / squared / bilinear terms.
        vector<TermFilterState> linear_filters, quadratic_filters, bilinear_filters;

        //! One composed MIN/MAX *constraint*, evaluated. Parallel to
        //! `composed_minmax_constraints`; the comparison and clause id stay on the plan.
        struct ComposedConstraint {
            vector<ComposedMinMaxTermData> terms;
            double rhs = 0.0;
            //! Its repair group, numbered after every clause of `constraints` and
            //! `absorbed_constraints`, so its slack is shared with no other clause.
            idx_t repair_group_id = DConstants::INVALID_INDEX;
            //! The classes its one row stands for (SolverInput::class_sets), under
            //! DIAGNOSE; INVALID_INDEX for none.
            idx_t class_set = DConstants::INVALID_INDEX;
        };
        vector<ComposedConstraint> composed_constraints;
        //! The composed MIN/MAX *objective*'s terms, evaluated.
        vector<ComposedMinMaxTermData> composed_objective_terms;
    };

    //! PHASE 2, sub-phase 2: evaluate the objective's coefficients onto gstate's
    //! objective-evaluation fields.
    EvaluatedClauses EvaluateObjective(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                                       ChunkExprCache &chunk_expr_cache, PerGroupCache &per_group_cache,
                                       const vector<EntityMapping> &entity_mappings) const;

    //! PHASE 2, sub-phase 3: evaluate every composed MIN/MAX clause — each inner term's
    //! per-row coefficient, the factor peeled off a reducer, the term's WHEN mask, the
    //! constant RHS. The rows these become are emitted later, by the formulation pass.
    void EvaluateComposedClauses(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                                 ChunkExprCache &chunk_expr_cache, PerGroupCache &per_group_cache,
                                 const vector<EntityMapping> &entity_mappings,
                                 EvaluatedClauses &evaluated) const;

    //! PHASE 2, before the clauses: evaluate every helper decision's members -- the rows of
    //! the reducer it holds, among those the clause and the reducer keep, grouped by its `by`
    //! key, or what each window of a range frame reads -- listed onto
    //! gstate.evaluated_value_links (ReducerValueMembers), innermost first. A member stands
    //! where the frames that may drop it select (its link's `when`) and every helper it
    //! reads is present; an instance left with no member is absent (gstate.helper_present),
    //! which the clauses and the objective read. The formulation pass states the
    //! definition over the members.
    void EvaluateReducerValueLinks(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                                   ChunkExprCache &chunk_expr_cache, PerGroupCache &per_group_cache,
                                   const vector<EntityMapping> &entity_mappings) const;
    //! A range frame's helper (`link.frame`), into `out`: one group per window that reads
    //! something, read at its anchor; a member per row the window reads, and one for its
    //! `else` where a position is missing (syntax_reference.md §5.6).
    void EvaluateFrameLink(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                           ChunkExprCache &chunk_expr_cache, const LogicalDecide::ReducerValueLink &link,
                           ReducerValueMembers &out) const;

    //! PHASE 3a, assembly: build `SolverInput` out of what PHASE 2 evaluated — variable
    //! types and the column box, implied-bound propagation, the objective transfer with
    //! its qualifier de-duplication and AVG scaling, and the flat column space. Runs
    //! once per query: it moves the evaluated data off `gstate`, and the box it settles
    //! is the one the formulation pass below is then handed.
    //! `evaluated` is taken mutably because this is where the objective's per-term
    //! filter masks are *finalized*: the relation qualifier's de-duplication is folded
    //! into them before AVG scaling reads the surviving-row counts. The formulation pass
    //! below does not read those masks, so a second formulation is unaffected.
    SolverInput BuildSolverInput(DecideGlobalSinkState &gstate, idx_t num_rows,
                                 vector<EntityMapping> entity_mappings, EvaluatedClauses &evaluated,
                                 VarIndexer &out_var_indexer) const;

    //! PHASE 3b, formulation: turn the evaluated clauses into solver rows against `box`
    //! — the auto-`M` refill, every linearization pass, and the lowering of whatever the
    //! chosen backend cannot state natively.
    //!
    //! A pure function of its arguments: no `ClientContext`, no data scan, no expression
    //! evaluation. That is what lets it run a second time, on a copy of `input` against a
    //! widened box, to derive the constants an elastic (infeasibility-repair) model needs
    //! — see `FormulateElasticModel`. Keep it that way: anything here that reads the data
    //! instead of `input` belongs in PHASE 2.
    void FormulateModel(SolverInput &input, const FormulationBox &box, VarIndexer &var_indexer,
                        const EvaluatedClauses &evaluated) const;

    //! The model input and column space as PHASE 3a left them — before a single Big-M or
    //! McCormick constant was derived — plus the scoped bounds the column box enforces
    //! (`evaluated_absorbed_constraints`), whose rows only a repair model carries. Retained
    //! only under `DIAGNOSE`, so the INFEASIBLE terminal can formulate a *second* model
    //! against the widened box a repair searches in, instead of re-using constants that
    //! were derived against the narrower box the solved model declares.
    struct ElasticFormulation {
        SolverInput input;
        VarIndexer var_indexer;
    };

    //! PHASE 3, solve + readback: build_var_labels, the RouteSolveResult switch and its
    //! four terminal cases, and the shared success epilogue.
    SinkFinalizeType FinalizeSolveResult(ClientContext &context, DecideGlobalSinkState &gstate,
                                         SolverInput &solver_input, VarIndexer &var_indexer,
                                         const EvaluatedClauses &evaluated, ElasticFormulation *elastic,
                                         bool bench, Profiler &model_timer, Profiler &solver_timer) const;

    //! Formulate the elastic model: widen every loosenable column direction, re-derive
    //! every constant against that box, and build the solver model from it. `elastic` is
    //! left untouched — the copy it works on is local.
    SolverModel FormulateElasticModel(const ElasticFormulation &elastic,
                                      const EvaluatedClauses &evaluated,
                                      bool tolerate_infeasible_bounds, VarIndexer &out_var_indexer,
                                      vector<string> &out_global_labels) const;
};

} // namespace duckdb
