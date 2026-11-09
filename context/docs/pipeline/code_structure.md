# Codebase structure

Where DeciDB lives inside the DuckDB source tree, and which stage owns each file.
Stage numbers refer to the stage table in [`../README.md`](../README.md).

---

## 1. File organization

### Headers (`src/include/duckdb/`)

| Path | Stage | Contents |
|---|---|---|
| `common/enums/decide.hpp` | all | `DecideSense`, `DecideExpression`, `DecideVarScope` (`ROW` / `ENTITY` / `SCALAR`) and `DecideVarScopeInfo`, `DecideScopeCheckKind`, `ConstraintKind`, every DECIDE tag constant and the tag helpers |
| `common/decide_source_info.hpp` | 03 | `DecideSourceText` — the DECIDE tail's text and spans; `ConstraintSourceInfo` — the source display registry entry |
| `common/decide_solver_capabilities.hpp` | 07 answers, 03/05/06/08 read | `SolverConstructSupport`, `SolverModelClass`, `DecideModelShape` and `ClassifyDecideModel` (the one classifier of a model's shape), `IsNonconvexProductGroup` — the backend differences upstream stages branch on. In `common/` because stage 07 answers it but four stages above read it |
| `parser/decide/decide_parsed_markers.hpp` | 01, 02 read | Predicates over the parsed DECIDE markers: `IsDecideMarker`, `IsDecideScopeMarker`, `IsDecideAggregateName`, `IsScopedReducerCall`, `TakesReducerBy`, `UnwrapReducerMarkers`, `ReducerArgumentBody` |
| `parser/decide/decide_removed_spellings.hpp` | 01 | `RefuseRemovedDecideSpellings` |
| `planner/expression_binder/decide/decide_binder.hpp` | 02 | Base decision binder; `ValidateSumArgument`, `ValidateDecideScopeSpellings`, `FindOrCreateKeyScope`, `ValidateDecideNoExplicitDecisionCasts`, `ValidateDecideNoDataAggregateFactor`, the integrality gate and `DecideValueIsWholeNumbered` |
| `planner/expression_binder/decide/decide_constraints_binder.hpp` | 02 | `SUCH THAT` |
| `planner/expression_binder/decide/decide_objective_binder.hpp` | 02 | `MAXIMIZE` / `MINIMIZE` |
| `planner/expression_binder/decide/decide_declarations_binder.hpp` | 02 | `DecideDeclarationsBinder` — the whole DECIDE clause: declarations, scopes, then `SUCH THAT` and the objective |
| `planner/operator/decide/logical_decide.hpp` | 03 | `LogicalDecide`, `KeyedScopeInfo`, every metadata field |
| `planner/operator/decide/logical_decide_diagnose.hpp` | 03 | `LogicalDecideDiagnose` — the `DIAGNOSE <select>` plan node and the shape of the relation it returns |
| `planner/decide/decide_canonicalizer.hpp` | 04 | The canonical contract, in code |
| `planner/decide/decide_product_form.hpp` | 04 | The one spelling of a product of decisions: `NormalizeProducts`, `ReadDecideProduct`, `BuildDecideProduct`, `DecideDegreeOf` — the one definition of polynomial degree |
| `planner/decide/decide_shape_check.hpp` | 04 | `DecideShapeCheck` — the one place the algebra of a clause is judged |
| `planner/decide/decide_term_split.hpp` | 04/05 | `DecideTermSplitter` — reads one atom's body in the one spelling as terms; never refuses |
| `planner/decide/decide_constraint_walk.hpp` | 04 | Which children of a node are constraints: the predicates for the `when`, `per K:` and two-level objective wrappers, and the constraint-position walk every stage shares |
| `planner/decide/decide_source_provenance.hpp` | 03 | Source capture and rendering |
| `planner/decide/decide_cast_policy.hpp` | 04 | `UnwrapDecideCasts`, `StripCastsForIdentity`, `GetBareDecideColumnRef` |
| `planner/decide/decide_prepared_model.hpp` | 05/08 | The prepared linear form — stage 05's flattened terms and `DecideProductGroup`s, stage 08's coefficients |
| `optimizer/decide/decide_optimizer.hpp` | 05 | The rewrite passes |
| `optimizer/decide/decide_linear_form.hpp` | 05 | Flattening into `decide.prepared`; the last DECIDE optimization pass |
| `optimizer/decide/decide_solver_gate.hpp` | 05 | Backend choice, the native-construct mask, and the plan-time model-class gate |
| `optimizer/decide/decide_optimizer_internal.hpp` | 05 | Helpers shared by the `decide_rewrite_*.cpp` passes; internal to `src/optimizer/decide/` |
| `execution/operator/decide/physical_decide.hpp` | 08 | `PhysicalDecide`, `Term`, `DecideConstraint`, `Objective` |
| `decidb/formulation/solver_input.hpp` | 06/08 | `SolverInput`, `EvaluatedConstraint`, `EvaluatedProductGroup`, `CoefficientColumn`, `KeyedMapping` |
| `decidb/formulation/ilp_model.hpp` | 06 | `VarIndexer`, `SolverModel`, `ModelConstraint`, `ConstraintProvenance`, `SparseCoeffAccumulator` |
| `decidb/formulation/ilp_linearization.hpp` | 06 | `LowerDecideConstructs` and the Big-M derivations: a tagged constraint becomes solver rows |
| `decidb/formulation/ilp_linearization_internal.hpp` | 06 | Helpers shared by the `linearization_*.cpp` passes; internal to `src/decidb/formulation/` |
| `decidb/solver/ilp_solver.hpp` | 07 | `SolveModel` facade, `SolverBackend`, `SolveModelOptions` |
| `decidb/solver/solver_result.hpp` | 07 | `SolverStatus`, `SolverResult`, `ThrowDecideSolveError` |
| `decidb/solver/solver_session.hpp` | 07 | `SolverSession` — warm continuation |
| `decidb/solver/solver_config.hpp` | 07 | Time limits, primary and diagnostic |
| `decidb/solver/solver_registry.hpp` | 07 | `SolverBackend` handle, `SolverRegistry` — the one place a backend is named |
| `decidb/solver/probe_models.hpp` | 07 | Zero-objective and ray-fallback probe models for diagnostic re-solves |
| `decidb/gurobi/gurobi_solver.hpp`, `gurobi_loader.hpp` | 07 | Gurobi backend and dynamic loading |
| `decidb/naive/deterministic_naive.hpp` | 07 | HiGHS backend — vendored, statically linked, the capability floor |
| `decidb/diagnostics/decide_diagnostic.hpp`, `decide_diagnostic_engines.hpp`, `decide_diagnostic_render.hpp`, `decide_router.hpp`, `diagnostic_constants.hpp` | — | Query diagnostics; see `../diagnostics/` |

### Sources (`src/`)

| Path | Stage | Contents |
|---|---|---|
| `parser/decide/decide_parsed_markers.cpp` | 01 | The parsed-marker predicates, shared by the removed-spelling refusal and the binder |
| `parser/decide/decide_removed_spellings.cpp` | 01 | Refuses the removed DECIDE spellings, naming the new spelling from the user's own text; called by the transformer and, for a stored parse tree, by `BindDeclarations` |
| `planner/expression_binder/decide/decide_binder.cpp` | 02 | Shared DECIDE expression rules, reducers, the colon form, a reducer's own scope and `by`, key scopes, where `when` / `per` / `by` may stand, the integrality gate |
| `planner/expression_binder/decide/decide_constraints_binder.cpp` | 02 | `SUCH THAT` |
| `planner/expression_binder/decide/decide_objective_binder.cpp` | 02 | Objective, its scope and the two-level form |
| `planner/expression_binder/decide/decide_declarations_binder.cpp` | 02 | DECIDE declarations and their scopes, the parsed-tree validators (`RefuseQualifiedDecisionReferences` among them), and the `SUCH THAT` / objective binds |
| `planner/binder/query_node/bind_select_node.cpp` | 02 | Generic SELECT binding. Its DECIDE branch is one `DecideDeclarationsBinder::BindDeclarations()` call |
| `planner/binder/query_node/plan_select_node.cpp` | 03/04 | Subquery flattening, correlation provenance, the user canonicalization call and the shape check |
| `planner/operator/decide/logical_decide.cpp` | 03 | `AddConstraint`, `SetObjective`, EXPLAIN strings |
| `planner/operator/decide/logical_decide_diagnose.cpp` | 03 | The DIAGNOSE node: EXPLAIN strings and serialization |
| `planner/decide/decide_canonicalizer.cpp` | 04 | The one shape boundary |
| `planner/decide/decide_product_form.cpp` | 04 | `NormalizeProducts` and the readers of the product spelling, `DecideDegreeOf` |
| `planner/decide/decide_shape_check.cpp` | 04 | `DecideShapeCheck`: vocabulary, exponents, divisors, `CASE` and degree, run once in `plan_select_node.cpp` |
| `planner/decide/decide_term_split.cpp` | 04/05 | `DecideTermSplitter` |
| `planner/decide/decide_source_provenance.cpp` | 03 | Source display capture and rendering |
| `planner/decide/decide_cast_policy.cpp` | 04 | Cast unwrapping |
| `optimizer/decide/decide_optimizer.cpp` | 05 | The pass dispatcher and the helpers the passes share |
| `optimizer/decide/decide_rewrite_rhs_bounds.cpp` | 05 | `RewriteRhsAggregates`: a data-only reducer on a bound's right side becomes a placeholder column, recorded for the path to build as a side table (`direct_bound_tables.cpp`); runs before the other passes |
| `optimizer/decide/direct/` | 05 | Direct solve: the IR, the rules (S1, A1, R1, N1, A2), and the plan fragments that replace the DECIDE node; `../direct_solve/README.md` |
| `optimizer/decide/decide_rewrite_norm_in.cpp` | 05 | `norm` and DECIDE-variable `IN` |
| `optimizer/decide/decide_rewrite_notequal_avg.cpp` | 05 | `<>` indicators and AVG→SUM |
| `optimizer/decide/decide_rewrite_abs.cpp` | 05 | ABS Big-M tagging and linearization |
| `optimizer/decide/decide_rewrite_minmax.cpp` | 05 | MIN/MAX, plain and composed |
| `optimizer/decide/decide_rewrite_bilinear.cpp` | 05 | Bilinear McCormick |
| `optimizer/decide/decide_rewrite_define.cpp` | 05 | `DefineNonlinearReducerBodies`: a helper decision per row for each non-linear body a MIN/MAX formulation reads |
| `optimizer/decide/decide_rewrite_scoped.cpp` | 05 | The value rewrite (`RewriteScopedReducers`): each decision reducer of a VALUE-shape comparison becomes a helper decision per `by` group |
| `optimizer/decide/decide_singleton_row_folding.cpp` | 05 | Literal bounds folded into column boxes |
| `optimizer/decide/decide_linear_form.cpp` | 05 | Flattens every constraint and the objective into `decide.prepared`; must run last |
| `optimizer/decide/decide_answer_choice.cpp` | 05 | `ChooseDecideAnswer`: tries the direct-solve rules; when none applies, records the backend for the formulation passes (`ChooseDecideSolver`) |
| `optimizer/decide/decide_solver_gate.cpp` | 05 | `DeriveDecideModelClass`, `RequireDecideSolverSupport` |
| `execution/column_binding_resolver.cpp` | 03 | The `LOGICAL_DECIDE` case, with `ignored_bindings` |
| `optimizer/rule/case_simplification.cpp`, `optimizer/rule/arithmetic_simplification.cpp` | 05 | Edits to DuckDB's expression rewriter that direct plans rely on: a constant `WHEN` that fails to evaluate raises at run time, and the IEEE-exact float identities; `architecture.md` § 1 |
| `optimizer/statistics_propagator.cpp`, `optimizer/statistics/expression/propagate_columnref.cpp` | 05 | Statistics propagation folds an expression whose children it made constant, and reads a constant column of a projection as its value inside a computation; `architecture.md` § 1 |
| `optimizer/statistics/operator/propagate_join.cpp` | 05 | IS NOT DISTINCT FROM becomes `=` when neither side can hold a NULL; `architecture.md` § 1 |
| `optimizer/statistics/expression/propagate_case.cpp` | 05 | A `CASE` whose `WHEN` checks the statistics prove constant folds, by `CaseSimplificationRule::Simplify`, the fold the rewriter's rule makes; `architecture.md` § 1 |
| `execution/operator/join/perfect_hash_join_executor.cpp` | 08 | The perfect hash join takes the LEFT and SINGLE joins, with one NULL slot past the build range; `architecture.md` § 1 |
| `execution/physical_plan/plan_decide.cpp` | 03/08 | Logical → physical, entity key indices, verification |
| `execution/operator/decide/physical_decide.cpp` | 08 | Extraction, materialization, evaluation, emission, readback |
| `decidb/formulation/ilp_model_builder.cpp` | 06 | `SolverModel::Build` |
| `decidb/formulation/ilp_linearization.cpp` | 06 | `LowerDecideConstructs`, global-auxiliary allocation |
| `decidb/formulation/linearization_bigm.cpp` | 06 | Big-M sizing and the per-row range walks |
| `decidb/formulation/linearization_minmax.cpp` | 06 | MIN/MAX: constraints, links, objectives, composed |
| `decidb/formulation/linearization_not_equal.cpp` | 06 | `<>` collapse and Big-M disjunction |
| `decidb/formulation/linearization_bilinear_abs.cpp` | 06 | McCormick and ABS rows |
| `decidb/solver/ilp_solver.cpp` | 07 | Dispatch, INF_OR_UNBD probe, ray attachment |
| `decidb/solver/solver_registry.cpp` | 07 | `REGISTERED_BACKENDS` — the one table naming every backend |
| `decidb/solver/probe_models.cpp` | 07 | Probe models for the diagnostic re-solve and ray-extraction paths |
| `decidb/gurobi/gurobi_solver.cpp`, `gurobi_loader.cpp` | 07 | Gurobi backend |
| `decidb/naive/deterministic_naive.cpp` | 07 | HiGHS backend |
| `decidb/diagnostics/decide_diagnostic*.cpp`, `decide_router.cpp` | — | Diagnostics |

### Grammar (`third_party/libpg_query/`)

| Path | Contents |
|---|---|
| `third_party/libpg_query/grammar/statements/select.y` | Every DECIDE production, the error-only ones for the removed spellings included |
| `third_party/libpg_query/grammar/grammar.y` | The `%expect 4` conflict budget and its rationale; the DECIDE token declarations |
| `third_party/libpg_query/grammar/keywords/reserved_keywords.list` | `DECIDE`, `MAXIMIZE`, `MINIMIZE`, `SUCH` |
| `third_party/libpg_query/src_backend_parser_parser.cpp` | `base_yylex` — the DECIDE keyword tokens (`WHEN_DECIDE`, `WHEN_DECIDE_OBJECTIVE`, `PER_DECIDE`, `PER_DECIDE_PAREN`, `PER_DECIDE_ROW`, `BY_DECIDE`) |

---

## 2. Class hierarchy

### Binders

```mermaid
classDiagram
    class ExpressionBinder {
        +BindExpression()
    }
    class DecideBinder {
        +BindExpression()
        +BindAggregate()
        +BindFunction()
        +BindQualifiedReducer()
        +BindScopedReducer()
        +BindReducerBy()
    }
    class DecideConstraintsBinder {
        +BindComparison()
        +BindBetween()
        +BindOperator()
        +BindConjunction()
        +BindScopedConstraint()
        +GetExpressionType()
    }
    class DecideObjectiveBinder {
        +BindExpression()
        +BindScopedObjective()
        +BindTwoLevelObjective()
        +GetExpressionType()
    }

    ExpressionBinder <|-- DecideBinder
    DecideBinder <|-- DecideConstraintsBinder
    DecideBinder <|-- DecideObjectiveBinder
```

`ValidateSumArgument()`, `ValidateDecideScopeSpellings()`,
`ValidateDecideNoDataAggregateFactor()`, `ValidateDecideNoExplicitDecisionCasts()` and
`FindOrCreateKeyScope()` are free functions declared in `decide_binder.hpp`, not
methods.

### Operators and the model path

```mermaid
classDiagram
    class LogicalOperator
    class LogicalDecide {
        +decide_constraints
        +decide_objective
        +AddConstraint()
        +SetObjective()
        +Serialize()
    }
    class DecideCanonicalizer {
        +CanonicalizeTree()
        +CanonicalizeObjective()
        +VerifyCanonical()
        +ClassifyCanonicalComparison()
    }
    class DecideOptimizer {
        +Optimize()
    }
    class PhysicalOperator
    class PhysicalDecide {
        +Sink()
        +Finalize()
        +GetData()
    }
    class SolverInput {
        +constraints
        +objective_coefficients
        +keyed_mappings
    }
    class SolverModel {
        +col_lower / col_upper
        +constraints
        +Build(SolverInput, VarIndexer)$
    }
    class SolverSession {
        +Solve()
        +Continue()
    }

    LogicalOperator <|-- LogicalDecide
    PhysicalOperator <|-- PhysicalDecide
    LogicalDecide ..> DecideCanonicalizer : AddConstraint / SetObjective
    DecideOptimizer ..> LogicalDecide : rewrites
    LogicalDecide ..> PhysicalDecide : plan_decide.cpp
    PhysicalDecide ..> SolverInput : Finalize
    SolverInput ..> SolverModel : Build()
    SolverModel ..> SolverSession : Solve() / Continue()
```

---

## 3. Key entry points

### `src/parser/decide/decide_removed_spellings.cpp` — stage 01
- **`RefuseRemovedDecideSpellings()`** — the first removed spelling in a parsed clause,
  refused with a message that names its replacement.

### `src/planner/expression_binder/decide/decide_binder.cpp` — stage 02
- **`ValidateSumArgument()`** — what only the binder sees in a reducer's argument (a nested reducer over data, a
  subquery that is not scalar, a truth value over a decision); it judges no algebra.
- **`ValidateDecideScopeSpellings()`** — where `when`, `per` and `by` may stand.
- **`FindOrCreateKeyScope()`** — the one key scope per set of key columns.
- **`ValidateDecideNoExplicitDecisionCasts()`** — the cast authorship boundary.
- **`BindQualifiedReducer()` / `BindScopedReducer()` / `BindReducerBy()`**.

### `src/planner/decide/decide_canonicalizer.cpp` — stage 04
- **`CanonicalizeTree()` / `CanonicalizeObjective()`** — pure; a comparison is canonicalized inside the tree.
- **`Decompose()` / `PeelScale()` / `BuildAdditive()`** — shared by both clauses.
- **`ClassifyScoped()` / `ClassifyCanonicalComparison()`** — `PER_ROW` / `AGGREGATE` / `VALUE` / `INVALID`.
- **`BuildConstraintScopeChecks()` / `BuildObjectiveScopeChecks()`** — the run-time checks.
- **`VerifyCanonical()` / `VerifyCanonicalObjective()`** — non-mutating, throwing.

### `src/planner/decide/decide_product_form.cpp` — stage 04
- **`NormalizeProducts()`** — the bottom-up rewrite rule that writes every product of decisions in the one spelling.
- **`ReadDecideProduct()` / `BuildDecideProduct()`** — the reader of the spelling and the builder every pass that
  multiplies uses.
- **`DecideDegreeOf()`** — polynomial degree, not occurrence count: the one definition.

### `src/planner/decide/decide_shape_check.cpp` — stage 04
- **`DecideShapeCheck::CheckConstraints()` / `CheckObjective()`** — the algebra of each clause, judged once on the
  canonical spelling; every refusal quotes the clause as written and carries the term's location.

### `src/include/duckdb/planner/decide/decide_constraint_walk.hpp` — stage 04
Header-only, and the one place that answers "which children of this node are
constraints". A bound constraint tree is AND-conjunctions plus wrappers — `when c:`,
`per K:` and the two-level objective's `TWO_LEVEL_OBJECTIVE_TAG` — and a wrapper's trailing children are
metadata — the `when` condition, the key columns — so descending into them would read
a condition as another constraint.
- **`IsWhenFilterWrapper()` / `IsPerClassWrapper()` / `IsTwoLevelObjectiveWrapper()`
  (the two-level objective's) / `IsConstraintWrapper()` / `IsConstraintChild()`** —
  the predicates. Every site that walks a bound constraint tree asks these, including
  the few that deliberately act differently on the answer: singleton-row folding refuses to
  descend into a `when`, and the MIN/MAX objective rewrite unwraps the two-level wrapper as
  it goes. They bring their own descent, not their own definition of a wrapper, so a
  new wrapper kind is a one-line change here.
- **`VisitConstraintTree()`** — parents before children, skipping metadata, with a
  visitor that can stop the walk. Const and mutating overloads. Neither hands out
  the owning `unique_ptr`: a pass that reseats a node is deciding tree shape and
  belongs at stage 04's boundary, not inside a traversal.
- **`ForEachConstraintLeaf()`** — one call per model row (a comparison, a bound
  `IN`, a bare boolean decision, a placeholder from an earlier rewrite). The shape
  most consumers want.

### `src/planner/operator/decide/logical_decide.cpp` — stage 03
- **`AddConstraint()` / `SetObjective()`** — the only post-planning entry points.
- Serialization is not here: `LogicalDecide::Serialize()` / `Deserialize()` are generated
  into `src/storage/serialization/serialize_logical_operator.cpp` from
  `storage/serialization/logical_operator.json`.

### `src/planner/decide/decide_source_provenance.cpp` — stage 03
- **`DecideReplaces()`** — the location rule's one helper: a replacement takes the
  query location of the node it equals in value (architecture.md § 3).
- **`RenderDecideSource()`** — the one user-facing expression renderer, used by
  both EXPLAIN paths and by the diagnosis labels. It cuts a located node's span from
  `DecideSourceText` and reprints only a node with no location.
- **`CollectDecideExpressionStrings()`** — the EXPLAIN walker, shared by the
  logical and physical operators.
- **`InitializeConstraintSourceInfo()` / `FinalizeConstraintSourceInfo()`** — the
  per-clause display registry, indexed by `source_clause_id`.

### `src/optimizer/decide/decide_optimizer.cpp` — stage 05
- **`OptimizeDecide()`** — the pass sequence. Each pass body lives in a
  `decide_rewrite_*.cpp` sibling (or `decide_singleton_row_folding.cpp`); this file keeps
  the dispatcher and the helpers the passes share, declared in
  `decide_optimizer_internal.hpp`.

### `src/execution/operator/decide/physical_decide.cpp` — stage 08
- **`GetGlobalSinkState()`** — constructs the sink state, which checks each decision's
  folded box against its domain before any data arrives.
- **`Sink()` / `Combine()`** — materialize into a `ColumnDataCollection`.
- **`Finalize()`** — entity mappings, coefficient evaluation, model build, solve.
- **`GetData()`** — readback with type-specific projection.

### `src/decidb/formulation/ilp_model_builder.cpp` — stage 06
- **`SolverModel::Build(SolverInput &, const VarIndexer &)`** — three linear
  constraint paths plus the quadratic builder, then the two native lists
  (general and indicator constraints) carried across in flat columns. Takes
  `SolverInput` by non-const reference so raw global constraints can be moved.
- **`SolverModel::ModelClass()`** — what the built model demands of a solver; the
  fact stage 05's plan-time prediction is checked against.
- **`SparseCoeffAccumulator`** — reusable scratch, dense or sorted-pairs.

### `src/decidb/solver/ilp_solver.cpp` — stage 07
- **`SelectSolverBackend()` / `SolveModel()`**. `SolveModel` takes the backend chosen
  at plan time rather than resolving one, and asserts the built model is within that
  backend's declared capabilities before loading it.

### `src/decidb/solver/solver_registry.cpp` — stage 07
- **`REGISTERED_BACKENDS`** — the one table naming every backend: identifier, display
  name, availability probe, capabilities, session factory. Adding a backend is adding a
  row; nothing else in the tree branches on which one is in play.
- **`SolverBackend`** — the handle the pipeline passes around, and
  **`SolverRegistry::Find` / `Backends` / `BackendsSupporting`**.

### `src/optimizer/decide/decide_solver_gate.cpp` — stage 05
- **`DeriveDecideModelClass()` / `RequireDecideSolverSupport()`** — the plan-time model-class gate
  (`ClassifyDecideModel` over the prepared groups). Refuses, before a row is read, a query whose model class the chosen
  backend cannot take.

---

## 4. Keyed variables end to end

`per K: x(TYPE)` from the grammar to readback. A reducer's own `per K'` and the colon
form `SUM(D: e)` use the same key scopes; only a declaration owns solver columns.

| Struct | Where | What |
|---|---|---|
| `DecideKey` | `logical_decide.hpp` | One entry of `LogicalDecide::keys`: `text` (the key as first written) and `elements` (`DecideKeyElement`: bound `expression`, `name`, `null_mode`) |
| `KeyedScopeInfo` | `logical_decide.hpp` | `key` (an index into `keys`), `scoped_variable_indices`, `value_link` |
| `KeyedMapping` | `solver_input.hpp` | `num_classes`, `row_to_class`, `absent_class` — built at execution |
| `VarIndexer` | `ilp_model.hpp` | The four-block layout and `Get` / `InstanceColumn` / `NumInstances` |

The path:

1. **Grammar** — `decide_per ':' ColId '(' variable_type ')'` in `select.y`, which
   wraps the name in the `SCOPE_PER_TAG` marker.
2. **Binder** — `FindOrCreateKeyScope` resolves each key element (a column, or a
   relation standing for its stored columns), finds or creates the key's entry of the
   key table and the one scope for that key, and `DecideVarScopeInfo::Keyed(idx, K)` records it (an `ENTITY`
   scope with its key as written).
3. **Logical plan** — the key table's elements keep the key columns alive through
   column pruning (`LogicalDecide::EnumerateExpressions`).
4. **Plan creation** — binding resolution turns each key element into a chunk position,
   as it does every expression the operator owns.
5. **Optimizer** — a helper decision of the value rewrite over a `by` key is an
   `ENTITY` decision too: its scope names its link (`value_link`) instead of key
   columns, and its entities are the `by` groups among the rows the link's clause keeps
   (over `by ()` it is query-wide). The other rewrites' auxiliaries are row-scoped; a
   MIN/MAX extremum is a global auxiliary stage 06 allocates.
6. **Execution PHASE 1.5** — composite NULL-safe key → class id → `row_to_class`;
   a NULL in a written key column is no class (the row maps to `absent_class`, fixed at 0), and a
   relation's columns compare NULL as a value.
7. **Model building** — one solver column per entity; coefficients from rows sharing
   an entity **accumulate** through `SparseCoeffAccumulator`.
8. **Readback** — `var_indexer.Get(var_idx, row)`, so every row of an entity gets the
   same value.
