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
#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/decidb/solver_capabilities.hpp"
#include "duckdb/planner/decide/decide_prepared_model.hpp"

namespace duckdb {

//! Tracks entity-scope metadata for decision variables scoped to a base table, or
//! for a reducer qualifier scoped to a set of relations at once (`sum(D, T: e)`).
//! When a variable is declared as "T.x IS BOOLEAN", it has one value per unique
//! row in table T, not per join result row. A multi-relation qualifier extends the
//! same idea: its tuple identity is the concatenation of every named relation's
//! own key, so de-duplication collapses only the fan-out contributed by relations
//! it does *not* name.
struct EntityScopeInfo {
    //! Table alias or name used in the DECIDE declaration (e.g., "S" or "Sensors").
    //! For a multi-relation qualifier, the named relations joined by ",", e.g. "D,T".
    string table_alias;
    //! DuckDB table index/indices from the bind context (Binding::index). A
    //! declaration scope always has exactly one; a qualifier scope has one per
    //! named relation.
    vector<idx_t> source_table_indices;
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

    //! Stable source display registry, indexed by source_clause_id.
    vector<ConstraintSourceInfo> constraint_sources;

    //! The user's written spelling of every cast and scalar subquery in the DECIDE
    //! clause, indexed by source fragment id. Binding rewrites both beyond recognition,
    //! so RenderDecideSource replays them from here whenever a plan or a diagnosis
    //! shows the user their own clause.
    vector<string> source_fragments;

    // The optimization sense (MINIMIZE or MAXIMIZE)
    DecideSense decide_sense;

    // The bound objective function expression
    unique_ptr<Expression> decide_objective;

    // Additive constant peeled from the objective body by
    // DecideCanonicalizer::CanonicalizeObjective (e.g. the `3` in
    // `MAXIMIZE SUM(x) + 3`). The solver ignores this — it doesn't change
    // argmax/argmin — but it's preserved here so a future "report the objective
    // value" feature can add it back without losing information. SetObjective ADDS
    // to it, so a later optimizer rewrite cannot discard the user's constant.
    // Zero when nothing was peeled.
    double objective_constant_offset = 0.0;

    // Number of auxiliary variables at the end of decide_variables (created by binder and optimizer)
    idx_t num_auxiliary_vars = 0;

    // Per-variable boolean flag: true if the variable was declared IS BOOLEAN.
    // Indexed by position in decide_variables. Auxiliary variables appended later
    // should also push_back their boolean status.
    vector<bool> is_boolean_var;

    // Indices of auxiliary indicator variables for not-equal (<>) constraints
    vector<string> ne_clause_labels;

    // Links from MIN/MAX indicator variables: (agg_name "min"/"max", indicator_idx)
    vector<string> minmax_clause_labels;

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
        //! Factor the canonicalizer peeled off this reducer (`2 * MAX(x*v)`), or nullptr
        //! for none. It stays OUTSIDE the reducer: MIN/MAX are order statistics, so
        //! pushing a factor in only commutes for a positive one, and the value may not
        //! be known until the query runs. The physical layer evaluates it once (it is
        //! query-wide by construction) and multiplies it into this term's contribution.
        //!
        //! Its sign, when known at plan time, participates in `is_easy` exactly as
        //! `sign` does -- a scale of -2 flips the direction z is pushed just as a
        //! subtraction would. When the sign is NOT known, `is_easy` is forced false:
        //! the indicator layer pins z to the true MIN/MAX in both directions, which is
        //! correct whichever sign the factor turns out to have.
        unique_ptr<Expression> scale;
        //! true when the term was `AGG(...) / scale` rather than `scale * AGG(...)`.
        bool scale_divides = false;
        bool is_easy = true;                  // For MIN/MAX: easy (no Big-M) or hard (indicator).
        //! Entity scope this term's reducer is qualified by (`SUM(D: ...)`), or
        //! INVALID_INDEX when unqualified. Mirrors `Term::qualifier_scope_idx`; the
        //! physical layer folds the matching de-duplication mask into `filter`'s mask so a
        //! qualified reducer keeps its identity semantics inside a composed clause.
        idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;
        //! `inner_expr` flattened into additive terms by BuildDecidePreparedModel, so
        //! the composed emitter reads prepared terms like every other construct rather
        //! than re-deriving them once the rows are in.
        vector<DecideTerm> inner_terms;
    };
    struct ComposedMinMaxConstraint {
        vector<ComposedMinMaxTerm> terms;
        unique_ptr<Expression> rhs_expr;      // RHS expression (typically scalar constant)
        ExpressionType outer_cmp;             // Outer comparison (<=, >=, <, >)
        idx_t source_clause_id = DConstants::INVALID_INDEX;
    };
    vector<ComposedMinMaxConstraint> composed_minmax_constraints;

    //! Composed MIN/MAX objective: additive sum of SUM/AVG/MIN/MAX terms in the objective.
    //! Populated by DecideOptimizer::RewriteComposedMinMaxObjective. Empty when the
    //! objective is a single aggregate (handled by RewriteMinMaxObjective) or plain linear.
    vector<ComposedMinMaxTerm> composed_minmax_objective_terms;

    // --- Absorbed variable bounds (set by DecideOptimizer::AbsorbVariableBounds) ---

    //! A user-written simple bound that was folded into a decision column's box instead
    //! of being emitted as a model row. Kept so the infeasible diagnosis can re-emit it
    //! as a slackable row and quote it back as the user wrote it.
    struct UserBoundSpec {
        idx_t decide_var_idx; //!< index into decide_variables
        char sense;           //!< '<' (<= K), '>' (>= K), '=' (== K)
        double k;             //!< the (integer-strict-normalized) bound value
        //! True when the user wrote a strict `<` / `>` that was integer-normalized into
        //! `k` (e.g. `x < 10` → k=9). `typed_k` carries the user's original literal so the
        //! re-emitted row mirrors `ConstraintProvenance::strict` / `typed_k` and the
        //! infeasible diagnosis re-quotes the suggestion as `<` / `>` against it.
        bool strict = false;
        double typed_k = 0.0;
        idx_t source_clause_id = DConstants::INVALID_INDEX;
    };

    //! Sentinel for "no explicit lower bound was written". Not 0, so that an explicit
    //! negative lower bound (`x >= -5`, `BETWEEN -10 AND 10`, a negative IN minimum) is
    //! honored instead of being clamped up to 0. The std::max combiner still picks the
    //! tightest of several `>=` bounds, and physical Finalize resolves anything still at
    //! the sentinel to the default 0 floor.
    static constexpr double ABSORBED_LOWER_UNSET = -1e30;

    //! Per-decision-variable box, sized to decide_variables. Physical execution copies
    //! these into SolverInput instead of re-walking the constraint tree.
    vector<double> absorbed_lower_bounds;
    vector<double> absorbed_upper_bounds;
    //! Every absorbed user bound, in absorption order. A BETWEEN contributes two specs.
    vector<UserBoundSpec> user_absorbed_bounds;

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

    //! Per-variable scope assignment (row / entity / query-wide scalar).
    //! Defaults to row-scoped; entity_scope_idx indexes entity_scopes.
    vector<DecideVarScopeInfo> variable_scopes;

    //! BoundColumnRefExpressions for every entity-key column (flattened in scope order).
    //! These live here so that DuckDB's binder initial column_id selection AND the
    //! RemoveUnusedColumns pruner track them as live references. Without them,
    //! entity-key columns that aren't referenced elsewhere (SELECT/WHERE/
    //! constraints/objective) would be pruned from the table scan, silently
    //! collapsing distinct entities into whatever grouping happens to survive.
    //! Reached by EnumerateExpressions below like every other owned expression, so
    //! the column-binding resolver rewrites these to physical BoundReferenceExpressions
    //! the same way it does decide_constraints/decide_objective; plan_decide.cpp reads
    //! the resolved physical index straight off them.
    vector<unique_ptr<Expression>> entity_key_expressions;

    //! The registered name ("gurobi", "highs") of the solver this query will run on,
    //! chosen ONCE by ChooseDecideSolver before any rewrite pass and carried from here
    //! to PhysicalDecide. A NAME and not a handle: layer 3 describes the query, and a
    //! live backend handle here would let a logical plan open a solver session, which
    //! is stage 07's job. Stage 07 turns the name back into a backend when it is
    //! actually time to solve (SolverRegistry::Find).
    //!
    //! Empty until a solver is chosen. Deliberately NOT serialized: which solver a host
    //! has is a property of the host, not of the query, so a plan deserialized elsewhere
    //! re-resolves it rather than carrying a choice that machine may not be able to honor.
    string solver_backend_name;

    //! Stage 05's FORMULATION DECISION: which constructs this query leaves for the
    //! backend to state itself, and which it lowers into plain rows. Decided once, here,
    //! from the chosen backend's construct table — because choosing a formulation is
    //! stage 05's job and nothing below it may re-decide. Stage 08 READS these and
    //! translates; it never asks a backend what it supports.
    //!
    //! `SolverConstructSupport` is a table of yes/no per construct, so it is exactly the
    //! shape of the decision; reusing it keeps the question ("can the backend?") and the
    //! answer ("then don't lower it") spelled the same way rather than in two parallel
    //! vocabularies. All-false until a solver is chosen, which is also the value that
    //! reproduces the pre-capability lowering behaviour.
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

    // --- Prepared linear form (built by BuildDecidePreparedModel, stage 05) ---

    //! Every constraint and the objective, already flattened into additive terms.
    //! Physical execution evaluates each term's coefficient against the relational
    //! input; it does not re-derive the shape. Empty until the DECIDE optimizer's
    //! final pass fills it, and moved into PhysicalDecide at physical planning.
    DecidePreparedModel prepared;

public:
    //! Add a constraint to the SUCH THAT tree, canonicalizing it on the way in.
    //! This is the ONLY way a constraint may enter LogicalDecide after planning,
    //! and it exists so that constraints synthesized by DecideOptimizer (ABS
    //! envelopes, Big-M rows, McCormick links) arrive in the same canonical shape
    //! as the ones the user wrote. Together with the canonicalization performed in
    //! Binder::CreatePlan, it is one of exactly two call sites of
    //! DecideCanonicalizer -- do not add a third, and do not append to
    //! decide_constraints directly.
    void AddConstraint(ClientContext &context, unique_ptr<Expression> constraint);

    //! Replace the objective, canonicalizing it on the way in. This is the objective's
    //! counterpart to AddConstraint and the ONLY way DecideOptimizer may install a
    //! rewritten objective (AVG scaling, MIN/MAX auxiliaries, ABS envelopes, bilinear
    //! links). Before it existed the four rewrite sites assigned decide_objective
    //! directly, so optimizer output was never re-canonicalized or verified while
    //! constraint output always was.
    //!
    //! Any additive constant the rewrite leaves behind is ADDED to
    //! objective_constant_offset rather than replacing it, so the offset peeled from
    //! what the user wrote survives every later rewrite.
    void SetObjective(ClientContext &context, unique_ptr<Expression> objective);

    //! Calls back with every expression this operator owns: decide_variables,
    //! decide_constraints, decide_objective, the composed MIN/MAX constraint and
    //! objective terms, and entity_key_expressions. This is the single place that
    //! knows LogicalDecide's expression layout -- generic DuckDB passes (the
    //! LogicalOperatorVisitor dispatcher, ColumnBindingResolver, RemoveUnusedColumns)
    //! call through it instead of each re-listing these fields by hand, which is how
    //! a field (entity_key_expressions, composed_minmax_*) previously went missing
    //! from one visitor while another already had it.
    void EnumerateExpressions(const std::function<void(unique_ptr<Expression> *)> &callback);

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

};

} // namespace duckdb
