//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/operator/decide/logical_decide.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/decide_solver_capabilities.hpp"
#include "duckdb/planner/decide/decide_prepared_model.hpp"

namespace duckdb {

//! A key scope: the columns whose values decide which result rows share one decision.
//! `per K: x(TYPE)` keys on exactly the columns K names, possibly across relations; a
//! relation named as a key (`per D`, `sum(D: e)`) stands for every column of that
//! relation. One value per distinct tuple, so de-duplication collapses only the fan-out
//! contributed by columns the key does *not* name.
struct EntityScopeInfo {
    //! What the user wrote: the key of `per K:` ("D.depotID, S.customerID", "D", "()").
    string table_alias;
    //! DuckDB table index/indices from the bind context (Binding::index): every
    //! relation a key column comes from.
    vector<idx_t> source_table_indices;
    //! Column types for the entity key columns
    vector<LogicalType> entity_key_column_types;
    //! Physical column indices in the child's output data chunk.
    //! These are resolved during physical plan creation (plan_decide.cpp)
    //! from the logical column bindings by matching against the child's GetColumnBindings().
    vector<idx_t> entity_key_physical_indices;
    //! Logical column bindings (table_index, col_index) — used to resolve physical indices
    vector<ColumnBinding> entity_key_bindings;
    //! Which decide_variables indices this scope keys
    vector<idx_t> scoped_variable_indices;
    //! Each key column as a message names it (`D.depotID`), in `entity_key_bindings`
    //! order; for a helper decision's scope, the names of its link's `by` key.
    vector<string> key_names;
    //! A helper decision's scope (DecideOptimizer::RewriteScopedReducers): the link into
    //! `LogicalDecide::reducer_value_links` it holds. Its entities are the groups of the
    //! link's `by` key among the rows the link's clause keeps -- data expressions rather
    //! than key columns, so `entity_key_bindings` is empty and execution builds the
    //! mapping from the link. INVALID_INDEX for every scope the binder creates.
    //! Deliberately absent from the serialization: the optimizer creates these scopes,
    //! and an optimized plan is never serialized.
    idx_t value_link = DConstants::INVALID_INDEX;
    //! A relation-qualified reducer's scope of its own (DecideCanonicalizer::
    //! BuildConstraintSignatures): the classes of the reducer's key, each split by the
    //! values of its body's signature (D64, D65), an index into LogicalDecide::signatures.
    //! Its entities are data expressions rather than key columns, so `entity_key_bindings`
    //! is empty and execution builds the mapping from the signature. INVALID_INDEX for every
    //! other scope.
    idx_t signature = DConstants::INVALID_INDEX;

    //! Generated from `storage/serialization/nodes.json`. `entity_key_physical_indices`
    //! is deliberately absent: it is resolved at physical planning, after the only point
    //! a plan is ever serialized, so it is always empty on the wire.
    void Serialize(Serializer &serializer) const;
    static EntityScopeInfo Deserialize(Deserializer &deserializer);
};

//! A reducer's `by (key)`. At a class, the reducer reads the kept rows whose key value
//! equals the class's, NULL being a value. Reducers whose keys are the same set of
//! expressions share one entry, named by GROUP_KEY_TAG_PREFIX.
struct DecideGroupKey {
	//! The key's data expressions, in the order first written.
	vector<unique_ptr<Expression>> expressions;
	//! The key as written, for messages: `D.region, S.dispatchDay + S.transitDays`.
	string text;
	//! Each expression as a message names it (`D.region`), in `expressions` order; a
	//! relation written in the key names each of its columns.
	vector<string> names;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideGroupKey Deserialize(Deserializer &deserializer);
};

//! One run-time requirement of a clause
//! (syntax_reference.md §7.3): built by DecideCanonicalizer from the user's
//! clause, before any rewrite, and run by PhysicalDecide before any model is built.
struct DecideScopeCheck {
	DecideScopeCheckKind kind = DecideScopeCheckKind::KEEPS_A_ROW;
	//! The rows considered: those `rows` keeps, NULL dropping a row; null for every row.
	unique_ptr<Expression> rows;
	//! READS_A_ROW: the groups. Empty for one group.
	vector<unique_ptr<Expression>> key;
	//! READS_A_ROW: the reducer's own `when`.
	unique_ptr<Expression> filter;
	//! What the message quotes, as written. KEEPS_A_ROW: the condition. READS_A_ROW: the
	//! reducer (`sum(when S.priority: ship)`).
	string subject;
	//! How a message names each key expression it describes a group by
	//! (`D.depotID` in `D.depotID = 'D1'`), in key order: a prefix of `key`.
	vector<string> key_names;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideScopeCheck Deserialize(Deserializer &deserializer);
};

//! One field of a variant signature: a value the scoped expression's body reads once per
//! instance (syntax_reference.md §7.1; D64, D77).
struct DecideSignatureField {
	DecideSignatureFieldKind kind = DecideSignatureFieldKind::VALUE;
	//! VALUE: the data expression, a maximal subtree with neither a decision nor a reducer
	//! -- the data terms of one additive context summed into one (D77).
	unique_ptr<Expression> value;
	//! DECISION: the decision (index into decide_variables), per row or keyed.
	idx_t decision = DConstants::INVALID_INDEX;
	//! BY_KEY: the reducer's `by` key (index into group_keys).
	idx_t group_key = DConstants::INVALID_INDEX;
	//! The field as a message names it: `P.lim`, `ship`, `by (P.region)`.
	string text;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideSignatureField Deserialize(Deserializer &deserializer);
};

//! The variant signature of one scoped expression -- a `per K:` clause, a reducer with its
//! own `per` (or the colon form), or the objective (syntax_reference.md §7.1; D64): its
//! rows split into classes by `key`, and each class into one instance per distinct value
//! of `fields` over its rows. Built once by DecideCanonicalizer from the canonical clauses,
//! in place of the determination checks; only an expression with a field has one.
struct DecideSignature {
	//! The rows it reads: its `when`, and a reducer's own; null for every row. A field is
	//! never computed anywhere else.
	unique_ptr<Expression> rows;
	//! The `per` key's expressions; empty for `per ()`.
	vector<unique_ptr<Expression>> key;
	vector<DecideSignatureField> fields;
	//! How a plan names the scoped expression: a clause's key as written (`per P.depotID`),
	//! a reducer as written (`sum(per D: ...)`), `the objective`.
	string text;
	//! The clause it belongs to (LogicalDecide::constraint_sources); INVALID_INDEX for the
	//! objective's.
	idx_t source_clause_id = DConstants::INVALID_INDEX;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideSignature Deserialize(Deserializer &deserializer);
};

//! A source column's logical identity and the name the user wrote for it.
//! Keeping these together makes it impossible for the binding/name association to
//! drift through mismatched parallel vectors.
struct DecideSourceColumnName {
    ColumnBinding binding;
    string name;

    //! Generated from `storage/serialization/nodes.json`.
    void Serialize(Serializer &serializer) const;
    static DecideSourceColumnName Deserialize(Deserializer &deserializer);
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

    //! The objective as WRITTEN and as CANONICALIZED, the objective's counterpart to
    //! `constraint_sources`. The objective is one side of a comparison with no relation
    //! to orient, so it needs no registry -- there is exactly one of it -- but a plan
    //! that shows what became of a constraint owes the same account of the objective.
    //! Both are captured in Binder::CreatePlan around the single CanonicalizeObjective
    //! call. `canonical_objective` is empty when canonicalization changed nothing.
    string written_objective;
    string canonical_objective;

    //! The user's written spelling of every cast and scalar subquery in the DECIDE
    //! clause, indexed by source fragment id. Binding rewrites both beyond recognition,
    //! so RenderDecideSource replays them from here whenever a plan or a diagnosis
    //! shows the user their own clause.
    vector<string> source_fragments;

    //! The user's own name for every column reachable from the DECIDE input, captured
    //! from the BindContext where name resolution happens. This is the ONLY place the
    //! user's spelling exists for a source that has no catalog entry: a `t(a, b, c)`
    //! alias list over `(VALUES ...)`, a subquery, or a CTE is recorded on the binding
    //! and never reaches the plan, whose projection carries the binder's positional
    //! `col0` / `col1` placeholders instead. The unbounded diagnosis uses it to label a
    //! categorical slice with a name the user actually wrote.
    //!
    //! Each entry keeps the logical binding and its name together. The generated
    //! serializer carries the record directly.
    vector<DecideSourceColumnName> source_columns;

    // The optimization sense (MINIMIZE or MAXIMIZE)
    DecideSense decide_sense;

    // The bound objective function expression
    unique_ptr<Expression> decide_objective;

    //! The `DIAGNOSE` prefix, carried from the statement that was written. True means
    //! this query was asked to explain its failure: the operator arms the diagnosis
    //! engines and reports its findings as a relation instead of raising. False — the
    //! default, and every unprefixed query — means a failed solve reports its status
    //! and stops, and no diagnostic work is done or paid for.
    //!
    //! It is a property of the STATEMENT, not of the session. Nothing reads it back out
    //! of a setting; it travels parser → binder → here → PhysicalDecide.
    bool diagnose = false;

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
        idx_t source_clause_id = DConstants::INVALID_INDEX;
        idx_t removal_group_id = DConstants::INVALID_INDEX;
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
        ExpressionType outer_cmp;             // Outer comparison (<=, >=, <, >, =)
        idx_t source_clause_id = DConstants::INVALID_INDEX;
        idx_t removal_group_id = DConstants::INVALID_INDEX;
        //! The classes its one row stands for, which an expanded diagnosis reports one by
        //! one (D28) -- the values of its `per K:` key among the rows its `when` keeps
        //! (`per_key` NAMES_CLASSES; `per ()` names none), or those rows themselves with
        //! no `per` (NONE). `when` is the clause's own, also folded into each term's filter.
        DecideConstraint::PerKeyRole per_key = DecideConstraint::PerKeyRole::NONE;
        vector<unique_ptr<Expression>> class_key;
        unique_ptr<Expression> when;
    };
    vector<ComposedMinMaxConstraint> composed_minmax_constraints;

    //! A decision reducer of a scoped comparison in the VALUE shape
    //! (CanonicalConstraintClass::VALUE), replaced by the helper decision `value_var`
    //! that holds `agg([when c'] [per K']: e) by (Γ)` over the rows the clause keeps: one
    //! instance per Γ group of those rows, defined once by stage 06 -- an equality row per
    //! group for SUM/AVG, an extremum link per group for MIN/MAX. Produced by
    //! DecideOptimizer::RewriteScopedReducers; an optimized plan is never serialized.
    //!
    //! One link per occurrence of a reducer in a clause, shared by every row the rewrites
    //! before this one derived from that clause (an ABS envelope pair holds two copies):
    //! the sides a shared MIN/MAX helper needs are the union of its rows' needs.
    struct ReducerValueLink {
        idx_t value_var = DConstants::INVALID_INDEX;
        string agg_name;                   //!< "sum", "avg", "min" or "max"
        string label;                      //!< the reducer as the user wrote it, `by` included, for messages
        unique_ptr<Expression> inner_expr; //!< e, as bound; the ABS and bilinear rewrites applied
        unique_ptr<Expression> filter;     //!< the reducer's own `when`, or null
        //! Its own `per K'`: the key scope whose classes it counts once; INVALID_INDEX
        //! for per row.
        idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;
        //! Γ, into `group_keys`; INVALID_INDEX for `by ()`, every kept row.
        idx_t group_key = DConstants::INVALID_INDEX;
        unique_ptr<Expression> when; //!< the clause's `when`, or null
        //! MIN/MAX: the sides that pin the helper (ExtremumLinkSpec), read from the way the
        //! clause pushes it; both when the sign of its factor is not known before the query
        //! runs or the comparison is `=` or `<>`. Unused for SUM/AVG, whose row is an equality.
        bool need_envelope = false;
        bool need_closing = false;
        idx_t source_clause_id = DConstants::INVALID_INDEX;
        idx_t removal_group_id = DConstants::INVALID_INDEX;
        //! `inner_expr` flattened by BuildDecidePreparedModel, like a composed term's.
        vector<DecideTerm> inner_terms;
    };
    vector<ReducerValueLink> reducer_value_links;

    //! What a rewrite made of a construct, for EXPLAIN: the construct as a plan prints it
    //! (`subject`) and what it became (`kind`), worded by the renderer in one place
    //! (RenderDecideRewriteAccount). One per construct a rewrite replaced, under the
    //! clause it came from; INVALID_INDEX is the objective. Produced by the rewrites; an
    //! optimized plan is never serialized.
    struct RewriteAccount {
        enum class Kind : uint8_t {
            AVG_SUM,                      //!< an AVG over decisions, a sum scaled by its row count
            NORM_SUM_ABS,                 //!< `norm(e, 1)`, a sum of absolute values
            NORM_SUM_SQUARES,             //!< `norm(e, 2)`, a sum of squares
            NORM_MAX_ABS,                 //!< `norm(e, 'inf')`, a maximum of absolute values
            EXTREMUM_EACH_MEMBER,         //!< a MIN/MAX in its cheap direction, a bound on each member
            ABS_AUXILIARY,                //!< an ABS held by solver pressure alone
            ABS_AUXILIARY_SIGN_INDICATOR, //!< an ABS pinned by Big-M rows and a sign binary
            ABS_AUXILIARY_NATIVE,         //!< an ABS stated to the solver as an absolute value
            PRODUCT_AND,                  //!< a Boolean product, AND rows
            PRODUCT_MCCORMICK,            //!< a Boolean times a bounded decision, McCormick rows
            IN_INDICATORS,                //!< `x IN (...)`, one indicator per value
            L0_INDICATORS,                //!< `norm(e, 0)`, one indicator per row
            VALUE_HELPER,                 //!< a reducer held by one helper decision
            VALUE_HELPER_PER_GROUP,       //!< a reducer held by a helper decision per `by` group
            EXTREMUM,                     //!< a MIN/MAX held by its envelope
            EXTREMUM_PINNED,              //!< a MIN/MAX pinned by indicator rows
            NOT_EQUAL,                    //!< `<>`, two conditional rows with an indicator
            //! What the data made of a scoped expression's classes (D64): more instances
            //! than classes. Recorded by execution, so only EXPLAIN ANALYZE prints it, and
            //! `subject` is the whole line.
            VARIANTS,
        };
        idx_t source_clause_id = DConstants::INVALID_INDEX;
        Kind kind = Kind::ABS_AUXILIARY;
        string subject;
    };
    vector<RewriteAccount> rewrite_accounts;

    //! Composed MIN/MAX objective: additive sum of SUM/AVG/MIN/MAX terms in the objective.
    //! Populated by DecideOptimizer::RewriteComposedMinMaxObjective. Empty when the
    //! objective is a single aggregate (handled by RewriteMinMaxObjective) or plain linear.
    vector<ComposedMinMaxTerm> composed_minmax_objective_terms;

    // --- Absorbed variable bounds (set by DecideOptimizer::AbsorbVariableBounds) ---

    //! A user-written simple bound that was folded into a decision column's box instead
    //! of being emitted as a model row. Recorded so the linear form can check that its
    //! clause was flattened for the repair model (ABSORBED_CLAUSE_ROWS_TAG), which is what
    //! gives the diagnosis its rows and quotes the bound as the user wrote it.
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
        idx_t removal_group_id = DConstants::INVALID_INDEX;
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

    // A MIN/MAX objective: original aggregate type before rewrite to SUM. A nested one is
    // a helper decision instead (RewriteScopedReducers, D75).
    ObjectiveAggregateType flat_objective_agg = ObjectiveAggregateType::NONE;
    // Pre-computed: true if easy formulation (MAXIMIZE+MIN or MINIMIZE+MAX)
    bool flat_objective_is_easy = false;

    // --- Key scopes ---

    //! Every key scope: of `per K:` and `sum(T: ...)`, one per key. Empty when no decision
    //! or reducer is keyed.
    vector<EntityScopeInfo> entity_scopes;

    //! Per-variable scope assignment (row / entity / query-wide scalar).
    //! Defaults to row-scoped; entity_scope_idx indexes entity_scopes.
    vector<DecideVarScopeInfo> variable_scopes;

    //! Every `by (key)` written in the clause, indexed by GROUP_KEY_TAG_PREFIX tags.
    vector<DecideGroupKey> group_keys;

    //! The run-time requirements of every clause, in
    //! clause order; PhysicalDecide runs them before it formulates anything.
    vector<DecideScopeCheck> scope_checks;
    //! The variant signature of every scoped expression with a field (D64), indexed by
    //! SIGNATURE_TAG_PREFIX on a `per K:` wrapper and by EntityScopeInfo::signature for a
    //! reducer's own `per`. Built by the planner with the scope checks.
    vector<DecideSignature> signatures;
    //! The objective's signature: it is produced once, so execution refuses a second
    //! instance (D66). INVALID_INDEX when the objective has no field.
    idx_t objective_signature = DConstants::INVALID_INDEX;

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

    //! True once DecideOptimizer has started rewriting this node.
    //!
    //! It is what makes the serialization contract enforceable: the wire format carries
    //! the BOUND plan — what the binder and the canonicalizer produced — and nothing
    //! this flag guards. Stage 05's output is a formulation chosen from the constructs
    //! THIS HOST's solver declares, which is the same reason `solver_backend_name` and
    //! `use_native_constructs` below are not serialized either. An optimized DECIDE plan
    //! is not portable by construction, so it declines to be copied rather than
    //! arriving somewhere else as a formulation that host cannot honor.
    //!
    //! Not serialized: anything deserialized is a bound plan, so false is the only
    //! value a fresh node can have.
    bool optimized = false;

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
    //! objective terms, the helper decisions' reducers, entity_key_expressions, the `by`
    //! keys and the scope checks.
    //! This is the single place that
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

    //! Serialization is defined by `storage/serialization/logical_operator.json` and
    //! generated into `serialize_logical_operator.cpp`; there is no hand-written copy.
    void Serialize(Serializer &serializer) const override;
    static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

    //! Bound plans only — see `optimized` above.
    bool SupportSerialization() const override {
        return !optimized;
    }
    
protected:
    // The table indices that this operator produces
    vector<idx_t> GetTableIndex() const override;

};

} // namespace duckdb
