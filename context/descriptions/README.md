# DeciDB Documentation Index

All internal documentation for the DeciDB project, structured for quick navigation
by both AI agents and human developers.

---

## Convention: `done.md` / `todo.md`

Every area is a **folder** containing:

- **`done.md`** — what is implemented today: semantics, implementation notes, code pointers
- **`todo.md`** — what remains: design rationale, the decision that needs making, a test, and the `done.md` section to update

The two are **disjoint by construction**. The moment a task ships, its content
moves out of `todo.md` and into `done.md`, rewritten as present-tense "how it
works" documentation — never a completed-item checklist.

Three documents in `01_pipeline/` are exceptions: `architecture.md`,
`code_structure.md` and `trace_life_of_a_query.md` describe the pipeline rather
than being stages of it, so they are flat files with no `todo.md`.

Cross-cutting issues that no single stage owns live in `06_issues/`. A stage's
`todo.md` holds only work that stage owns, and links to `06_issues/` rather than
restating an entry.

---

## Folder map

| Folder | Contains | Start here if… |
|---|---|---|
| `00_project_overview/` | What DeciDB is, and the DECIDE syntax reference (the canonical spec) | You are new, or need to know what queries are valid |
| `01_pipeline/` | The eight pipeline stages, each a folder with `done.md`/`todo.md`, plus architecture, source map and an end-to-end trace | You are working on or debugging any part of the DECIDE query path |
| `02_operations/` | Oracle testing methodology, release workflow, benchmarking, pip packaging | You need to run tests, cut a release, benchmark, or build the wheel |
| `03_expressivity/` | The DECIQL surface — each keyword or construct a folder with `done.md`/`todo.md`. Also `problem_types/` (LP/ILP/QP classification), `explain/` and `diagnose/` | You want to know whether a construct is valid, or are implementing one |
| `04_testing/` | Test coverage tracking — which scenarios are oracle-verified vs feasibility-only, and what gaps remain | You are adding tests, auditing coverage, or chasing a suspected regression |
| `05_performance/` | Append-only log of applied performance work. One dated file per batch, with hypothesis and measured outcome | You want to know what has already been tried, or are about to commit a perf change |
| `06_issues/` | `bugs/todo.md` (open defects) and `code_quality/todo.md` (duplication, dead code, fragile patterns) | You hit an unexpected error, or want the known traps before touching grammar/solver/linearization code |
| `07_query_diagnostics/` | Diagnosing failed or useless solves (infeasible / unbounded / slow): the elastic relaxation engine, ray diagnosis, and the shared `foundations/` plumbing. Start at its `README.md` | You are turning a solver failure into an actionable, least-change diagnosis |

---

## The pipeline at a glance

Eight stages, matching the ownership layers in `.claude/CLAUDE.md`. Full map and
flow diagram: [`01_pipeline/README.md`](01_pipeline/README.md).

| # | Stage | Owns | Key source |
|---|---|---|---|
| 01 | [parser](01_pipeline/01_parser/done.md) | Grammar, DECIDE `WHEN` lexing, transformer, desugaring | `grammar/statements/select.y`, `bind_select_node.cpp` |
| 02 | [binder](01_pipeline/02_binder/done.md) | Names, scopes, types, degree, reducers, DECIDE validity | `decide_binder.cpp`, `decide_constraints_binder.cpp` |
| 03 | [logical_plan](01_pipeline/03_logical_plan/done.md) | `LogicalDecide`, subquery provenance, `AddConstraint`/`SetObjective`, serialization | `logical_decide.cpp`, `plan_select_node.cpp` |
| 04 | [canonicalizer](01_pipeline/04_canonicalizer/done.md) | The one shape boundary — decisions left, bound right, one spelling per reducer scale | `decide_canonicalizer.cpp` |
| 05 | [optimizer](01_pipeline/05_optimizer/done.md) | Formulation choice: ABS, MIN/MAX, AVG, `<>`, bilinear, composed reducers | `decide_optimizer.cpp` |
| 06 | [model_formulation](01_pipeline/06_model_formulation/done.md) | `SolverInput` → `SolverModel`: layout, accumulation, bounds, Q matrix | `ilp_model_builder.cpp` |
| 07 | [solver](01_pipeline/07_solver/done.md) | Backend dispatch, Gurobi and HiGHS translation, status normalization | `ilp_solver.cpp`, `gurobi_solver.cpp`, `deterministic_naive.cpp` |
| 08 | [execution](01_pipeline/08_execution/done.md) | Materialization, extraction, coefficient evaluation, entity mapping, readback | `physical_decide.cpp` |

---

## Reading order from scratch

1. `00_project_overview/project_description.md` — what DeciDB is and why.
2. `00_project_overview/syntax_reference.md` — what you can write in a DECIDE clause.
3. `01_pipeline/architecture.md` — how the extension attaches to DuckDB.
4. `01_pipeline/trace_life_of_a_query.md` — one query through all eight stages.
5. Individual stage `done.md` files as needed.

`01_pipeline/code_structure.md` is only needed when you are about to edit source
and need to know where a concern lives.

---

## Authoritative sources (anti-redundancy rule)

- **DECIDE syntax** — `00_project_overview/syntax_reference.md` is the canonical
  spec. Feature docs in `03_expressivity/` deliberately do **not** restate syntax;
  they hold semantics, implementation notes and code pointers. When adding or
  changing syntax, update the spec first, then the feature doc.
- **Constraint and objective shape** — `01_pipeline/04_canonicalizer/done.md`. No
  other document says where a term is allowed to sit.
- **Linearization mechanics** (MIN/MAX easy/hard, ABS, `IN`, AVG scaling) —
  `03_expressivity/sql_functions/done.md` for user-facing semantics,
  `01_pipeline/05_optimizer/done.md` for the pass that implements them.
- **Stage-internal detail** — each stage doc covers only its own stage.
  `01_pipeline/02_binder/done.md` explains what the binder does with a declaration, not what
  the user writes.
- **Table-scoped variables** — syntax in `syntax_reference.md`; entity-mapping
  construction in `01_pipeline/08_execution/done.md`; `VarIndexer` in
  `01_pipeline/06_model_formulation/done.md`; the end-to-end path in
  `01_pipeline/code_structure.md` §4.
