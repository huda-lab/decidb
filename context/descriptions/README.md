# DeciDB Documentation Index

This folder contains all internal documentation for the DeciDB project. It is
structured for quick navigation by both AI agents and human developers.

---

## Convention: done.md / todo.md

In `03_expressivity/` and `04_optimizer/`, each feature area is a **subfolder** containing:

- **`done.md`** — What is implemented today: syntax, examples, and code pointers
- **`todo.md`** — What remains to be built: design rationale, implementation suggestions

This makes it trivial to determine what exists vs. what needs building.

---

## Folder Map

| Folder                 | Contains                                                                                                           | Start here if...                                                              |
| ---------------------- | ------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------- |
| `00_project_overview/` | What DeciDB is, the theory behind it, and the DECIDE syntax reference                                              | You are new to the project or need to understand what queries are valid       |
| `01_pipeline/`         | The query processing pipeline: architecture, each stage in order, source-code map, and a concrete end-to-end trace | You are working on or debugging any part of the DECIDE query path             |
| `02_operations/`       | Testing methodology, release workflow, benchmarking, pip packaging, and known limitations                           | You need to run tests, cut a release, benchmark performance, build the pip wheel, or check whether a feature is supported |
| `03_expressivity/`     | DECIQL keyword reference — each keyword is a subfolder with done.md/todo.md. Also includes `problem_types/` for LP/ILP/QP classification. | You want to know if a construct is valid, or are implementing a new keyword   |
| `04_optimizer/`        | COP optimizer strategies — each area is a subfolder with done.md/todo.md                                           | You are designing or implementing optimizer features                          |
| `05_testing/`          | Test coverage tracking — which scenarios are oracle-verified vs. feasibility-only, and what gaps remain            | You are adding tests, auditing coverage, or debugging a suspected correctness regression |
| `06_performance/`      | Append-only log of performance optimizations applied to DeciDB. One file per optimization batch, dated, with hypothesis + measured outcome. | You want to know what's already been tried, or you're about to commit a perf change and need to record it |
| `07_bugs/`             | Known open bugs with symptoms, reproductions, and what has been ruled out                                          | You hit an unexpected error or are looking for a starting point to fix something broken  |

---

## Recommended Reading Order (Starting From Scratch)

1. `00_project_overview/project_description.md` — What DeciDB is and why it exists.
2. `00_project_overview/syntax_reference.md` — What you can write in a DECIDE clause.
3. `01_pipeline/architecture.md` — How the system is structured and what the stages are.
4. `01_pipeline/trace_life_of_a_query.md` — A concrete example walking through every stage.
5. The individual stage docs (`01_parser.md`, `02_binder.md`, `03_execution.md`) as needed.
6. For execution details, the sub-docs (`03a` through `03e`) break down each phase.
7. `01_pipeline/04_explain.md` — EXPLAIN output, serialization, and profiling for the DECIDE operator.

You do not need to read `00_project_overview/background_theory.md` unless you want the
academic motivation. You do not need to read `01_pipeline/code_structure.md` unless you
are about to modify source code and need to know where things live on disk.

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

## Note on Redundancy

A few topics appear in more than one file. Authoritative sources:

- **DECIDE syntax** — `00_project_overview/syntax_reference.md` is the full spec.
- **IS BOOLEAN / IS INTEGER** — `syntax_reference.md` defines what the user writes.
  `02_binder.md` explains what the binder does with that declaration internally.
- **Linearity constraint** — appears in theory, parser, binder, and syntax docs, each
  scoped to that stage. The user-facing spec is in `syntax_reference.md`.
- **Table-scoped variables** — `syntax_reference.md` defines the user syntax (`table.var IS type`).
  `code_structure.md` Section 4 covers the key structs and code paths. `03b_coefficient_evaluation.md`
  describes the entity mapping construction (Phase 1.5). `03c_model_building.md` covers VarIndexer.
