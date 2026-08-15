# The DECIDE Pipeline

Eight stages carry a `DECIDE` query from SQL text to solved rows. Each is a
folder holding `done.md` (how the stage works today) and `todo.md` (what remains
open for that stage). The stage numbers are the same ownership layers declared in
`.claude/CLAUDE.md` — a change belongs to the stage that owns its behavior, not
to the stage that happens to call it first.

---

## Convention

A pipeline stage is a **folder** with `done.md` + `todo.md`. Three documents here
are not stages — they describe the pipeline rather than being part of it — and
stay as flat files:

| File | What it is |
|---|---|
| `architecture.md` | How DeciDB attaches to DuckDB, and the shape of the whole path |
| `code_structure.md` | Where things live on disk |
| `trace_life_of_a_query.md` | One concrete query walked end to end |

Cross-cutting issues that do not belong to a single stage live in
`../06_issues/`. A stage `todo.md` holds only work whose owner is that stage.

---

## Stages

| # | Stage | Owns | Primary source |
|---|---|---|---|
| 01 | [parser/](01_parser/) | Grammar, DECIDE `WHEN` lexing, the transformer, source-preserving parsed validation | `third_party/libpg_query/grammar/statements/select.y`, `src/planner/binder/query_node/bind_select_node.cpp` |
| 02 | [binder/](02_binder/) | Variable declarations, scope, types, polynomial degree, reducer recognition, DECIDE validity | `src/planner/expression_binder/decide_*.cpp` |
| 03 | [logical_plan/](03_logical_plan/) | `LogicalDecide`, subquery flattening and provenance, `AddConstraint` / `SetObjective`, serialization | `src/planner/operator/logical_decide.cpp`, `src/planner/binder/query_node/plan_select_node.cpp` |
| 04 | [canonicalizer/](04_canonicalizer/) | The one shape boundary: decisions left, bound right, one spelling for a reducer scale, C0–C7 / O0–O5 | `src/planner/decide/decide_canonicalizer.cpp` |
| 05 | [optimizer/](05_optimizer/) | Formulation choice: NORM, DECIDE-variable IN, ABS, MIN/MAX easy/hard, AVG→SUM, `<>`, bilinear McCormick, composed MIN/MAX | `src/optimizer/decide/decide_optimizer.cpp` |
| 06 | [model_formulation/](06_model_formulation/) | `SolverInput` → `SolverModel`: variable layout, coefficient accumulation, bounds | `src/decidb/utility/ilp_model_builder.cpp` |
| 07 | [solver/](07_solver/) | Backend dispatch, Gurobi and HiGHS translation, status normalization, time limits | `src/decidb/utility/ilp_solver.cpp`, `src/decidb/gurobi/` |
| 08 | [execution/](08_execution/) | Materialization, coefficient evaluation, entity mapping, readback | `src/execution/operator/decide/physical_decide.cpp` |

---

## Flow

```text
SQL text
   |  01 parser — grammar, DECIDE WHEN gating, transformer, desugaring
   v
parsed SelectNode with decide_variables / decide_constraints / decide_objective
   |  02 binder — names, scopes, types, degree, reducers; produces bound expressions
   v
BoundSelectNode
   |  03 logical plan — PlanSubqueries, correlation provenance
   |  04 canonicalizer — ONE shape decision for constraints AND objective, then VerifyCanonical
   v
LogicalDecide
   |  05 optimizer — formulation choice; every emitted row re-enters through
   |                 AddConstraint / SetObjective, which re-canonicalize and re-verify
   v
LogicalDecide (rewritten)
   |  VerifyCanonical / VerifyCanonicalObjective once more at physical-plan entry
   v
PhysicalDecide
   |  08 execution — Sink: materialize; Finalize: entity mappings, extraction, coefficients
   v
SolverInput
   |  06 model formulation — VarIndexer layout, coefficient accumulation
   v
SolverModel
   |  07 solver — Gurobi (preferred) or HiGHS, normalized status
   v
solution vector
   |  08 execution — GetData projects values back onto rows by scope
   v
result rows
```

Two things are worth reading off that diagram:

- **Canonicalization sits between planning and optimization, not inside either.**
  The optimizer assumes canonical input and returns canonical output; it never
  decides shape. This is why it is its own stage rather than a phase of stage 05.
- **Stages 06 and 07 run inside stage 08's `Finalize`.** They are separate stages
  because they own separate contracts (a solver-neutral model, and a backend
  translation), not because they run at separate times.

---

## Reading order

1. `../00_project_overview/project_description.md` — what DeciDB is.
2. `../00_project_overview/syntax_reference.md` — what a valid DECIDE query looks like.
3. `architecture.md` — how the extension attaches to DuckDB.
4. `trace_life_of_a_query.md` — one query through all eight stages.
5. The individual stage `done.md` files as needed.

`code_structure.md` is only needed when you are about to edit source and need to
know where a concern lives.

---

## Authoritative sources

- **DECIDE syntax** — `../00_project_overview/syntax_reference.md`. Stage docs do
  not restate syntax.
- **Constraint and objective shape** — `04_canonicalizer/done.md`. No other stage
  documents where a term is allowed to sit.
- **Linearization mechanics** (MIN/MAX easy/hard, ABS, IN, AVG scaling) —
  `../03_expressivity/sql_functions/done.md` for the user-facing semantics,
  [`05_optimizer/done.md`](05_optimizer/done.md) for the pass that implements them.
- **Stage-internal detail** — each stage doc covers only its own stage.
