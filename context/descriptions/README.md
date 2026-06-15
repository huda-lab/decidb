# DeciDB Documentation Index

This folder contains all internal documentation for the DeciDB project. It is
structured for quick navigation by both AI agents and human developers.

---

## Convention: done.md / todo.md

In `03_expressivity/`, `04_optimizer/`, `05_testing/`, and `08_query_diagnostics/`, each feature area is a **subfolder** containing:

- **`done.md`** — What is implemented today: semantics, implementation notes, and code pointers
- **`todo.md`** — What remains to be built: design rationale, implementation suggestions

This makes it trivial to determine what exists vs. what needs building.

---

## Folder Map

| Folder                 | Contains                                                                                                           | Start here if...                                                              |
| ---------------------- | ------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------- |
| `00_project_overview/` | What DeciDB is and the DECIDE syntax reference (the canonical syntax spec)                                         | You are new to the project or need to understand what queries are valid       |
| `01_pipeline/`         | The query processing pipeline: architecture, each stage in order, source-code map, and a concrete end-to-end trace | You are working on or debugging any part of the DECIDE query path             |
| `02_operations/`       | Testing methodology, release workflow, benchmarking, pip packaging, and known limitations                           | You need to run tests, cut a release, benchmark performance, build the pip wheel, or check whether a feature is supported |
| `03_expressivity/`     | DECIQL keyword reference — each keyword is a subfolder with done.md/todo.md. Also includes `problem_types/` for LP/ILP/QP classification. | You want to know if a construct is valid, or are implementing a new keyword   |
| `04_optimizer/`        | COP optimizer strategies — each area is a subfolder with done.md/todo.md                                           | You are designing or implementing optimizer features                          |
| `05_testing/`          | Test coverage tracking — which scenarios are oracle-verified vs. feasibility-only, and what gaps remain            | You are adding tests, auditing coverage, or debugging a suspected correctness regression |
| `06_performance/`      | Append-only log of performance optimizations applied to DeciDB. One file per optimization batch, dated, with hypothesis + measured outcome. | You want to know what's already been tried, or you're about to commit a perf change and need to record it |
| `07_issues/`           | Issue tracking: `bugs/` (open bugs in `todo.md`, lessons from resolved bugs in `done.md`) and `code_quality/` (open code-quality issues in `todo.md`) | You hit an unexpected error, want the known traps before touching grammar/solver/linearization code, or are looking for logged cleanup work |
| `08_query_diagnostics/` | Diagnosing failed/useless DECIDE solves (infeasible / unbounded / slow): the elastic relaxation engine, ray diagnosis, and the shared `foundations/` plumbing. Each state is a subfolder with done.md/todo.md; start at its `README.md`. | You are working on turning a solver failure into an actionable, least-change diagnosis |

---

## Recommended Reading Order (Starting From Scratch)

1. `00_project_overview/project_description.md` — What DeciDB is and why it exists.
2. `00_project_overview/syntax_reference.md` — What you can write in a DECIDE clause.
3. `01_pipeline/architecture.md` — How the system is structured and what the stages are.
4. `01_pipeline/trace_life_of_a_query.md` — A concrete example walking through every stage.
5. The individual stage docs (`01_parser.md`, `02_binder.md`, `03_execution.md`) as needed.
6. For execution details, the sub-docs (`03a` through `03e`) break down each phase.
7. `01_pipeline/04_explain.md` — EXPLAIN output, serialization, and profiling for the DECIDE operator.

You do not need to read `01_pipeline/code_structure.md` unless you are about to modify
source code and need to know where things live on disk.

---

## Quick Lookup: Pipeline Stage -> Doc -> Source

| Stage                  | What it does                                                              | Doc                           | Key source file(s)                                                                 |
| ---------------------- | ------------------------------------------------------------------------- | ----------------------------- | ---------------------------------------------------------------------------------- |
| Parser / Symbolic      | Normalizes algebraic expressions into canonical linear form               | `01_pipeline/01_parser.md`    | `src/decidb/symbolic/decide_symbolic.cpp`                                          |
| Binder                 | Validates linearity, binds decision variables, recognizes DECIDE aggregates | `01_pipeline/02_binder.md`    | `src/planner/expression_binder/decide_binder.cpp`, `decide_constraints_binder.cpp`, `bind_select_node.cpp` |
| Execution (overview)   | Pipeline overview with pointers to sub-phases                             | `01_pipeline/03_execution.md` | `src/execution/operator/decide/physical_decide.cpp`                                |
| — Expression Analysis  | Extracts `DecideConstraint`/`Objective` from bound expressions      | `01_pipeline/03a_expression_analysis.md` | `physical_decide.cpp` (DecideGlobalSinkState constructor)                 |
| — Coefficient Eval     | Evaluates coefficient expressions row-by-row, builds WHEN+PER groupings and aggregate-local filter masks  | `01_pipeline/03b_coefficient_evaluation.md` | `physical_decide.cpp` (Finalize)                                       |
| — Model Building       | Transforms `SolverInput` → `SolverModel`                                    | `01_pipeline/03c_model_building.md` | `src/decidb/utility/ilp_model_builder.cpp`                                   |
| — Solver Backends      | Gurobi (preferred) / HiGHS (fallback) dispatch                           | `01_pipeline/03d_solver_backends.md` | `ilp_solver.cpp`, `gurobi_solver.cpp`, `deterministic_naive.cpp`            |
| — Result Projection    | Projects solution values onto rows with type-specific casting             | `01_pipeline/03e_result_projection.md` | `physical_decide.cpp` (GetData)                                           |
| EXPLAIN                | EXPLAIN / EXPLAIN ANALYZE / FORMAT JSON output for DECIDE operator        | `01_pipeline/04_explain.md`            | `logical_decide.cpp`, `physical_decide.cpp`, `serialize_logical_operator.cpp` |
| Code Structure         | File organization, class hierarchy, key methods map                       | `01_pipeline/code_structure.md`        | All DeciDB source files                                                            |

> **Note**: Algebraic rewrites (AVG→SUM, ABS linearization, MIN/MAX classification, `<>` indicators) are performed by `DecideOptimizer` — see `04_optimizer/rewrite_passes/done.md`. The binder validates and binds expressions; the optimizer transforms them.

---

## Authoritative Sources (Anti-Redundancy Rule)

- **DECIDE syntax** — `00_project_overview/syntax_reference.md` is the canonical spec.
  Feature docs in `03_expressivity/` deliberately do **not** restate syntax; they hold
  semantics, implementation notes, and code pointers, with a pointer back to the spec.
  When adding or changing syntax, update the spec first, then the feature doc.
- **Linearization mechanics** (MIN/MAX easy/hard, ABS Path A/B, IN rewrite, AVG scaling) —
  `03_expressivity/sql_functions/done.md`. Other docs reference it rather than restating.
- **Stage-internal detail** — each `01_pipeline/` stage doc covers only its own stage;
  `02_binder.md` explains what the binder does with a declaration, not what the user writes.
- **Table-scoped variables** — syntax in `syntax_reference.md`; entity-mapping construction
  in `03b_coefficient_evaluation.md`; VarIndexer in `03c_model_building.md`; structs in
  `code_structure.md`.
