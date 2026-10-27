# DeciDB documentation

DeciDB extends DuckDB with constrained optimization in SQL. A `DECIDE` clause declares decisions over the rows of a
query, `SUCH THAT` states constraints, and `MAXIMIZE` or `MINIMIZE` states an objective. DeciDB turns the query into a
linear, mixed-integer or quadratic model, solves it in-process, and returns the optimum as ordinary rows. `WHERE`
filters one row at a time; `DECIDE` constrains the whole set of decisions at once. This generalizes package queries
(Brucato et al.), which select a subset of rows, to decisions of any type.

Gurobi is used when its library and license are available: it is faster on DeciDB workloads and supports every model
class. HiGHS is bundled and handles the classes in its capability table; a model only Gurobi can solve is refused rather
than sent to HiGHS. Some problems skip the solver altogether: direct solve answers them with ordinary relational
operators.

The language is specified by `context/New_syntax.pdf` ("the deck"). These docs describe what is built; deck features
not built yet are in [language/roadmap.md](language/roadmap.md).

## Map

| Path | Holds | Start here if you... |
|---|---|---|
| [language/](language/guide.md) | The DECIDE language: the guide, scopes, functions, problem types, `EXPLAIN`, `DIAGNOSE`, error messages, the decision log (D/S numbers) and the roadmap | want to know what you can write and what it means |
| [pipeline/](pipeline/architecture.md) | The eight stages from SQL text to solved rows, how DeciDB attaches to DuckDB, where code lives, one query traced end to end | are changing or debugging the DECIDE query path |
| [diagnostics/](diagnostics/README.md) | `DIAGNOSE` of an infeasible or unbounded query: the router, the elastic engine, ray diagnosis, shared foundations | are turning a solver failure into a smallest-edit diagnosis |
| [direct_solve/](direct_solve/README.md) | Answering a proved problem class with relational operators instead of a solver: architecture, one folder per class | are working on a solver bypass |
| [testing/](testing/README.md) | How the suite judges an answer, what each area covers, open gaps | are adding tests or auditing coverage |
| [performance/](performance/README.md) | Every benchmark, the measured findings, open questions | want to run a benchmark or plan performance work |
| [operations/](operations/README.md) | Release workflow, the Python package, cross-platform portability | are cutting a release or building the wheel |
| [issues/](issues/bugs.md) | [bugs.md](issues/bugs.md) (open defects) and [code_quality.md](issues/code_quality.md) (duplication, fragile patterns) | hit an unexpected result, or want the known traps |
| [glossary.md](glossary.md) | One name per concept, with its standard name | meet a term you do not know |
| [todo.md](todo.md) | The work queue: an ordered list of open items, each pointing to its owner's `todo.md` | are picking the next task |

## Pipeline stages

A change belongs to the stage that owns its behavior, not the stage that calls it first. The flow diagram is in
[pipeline/architecture.md](pipeline/architecture.md).

| # | Stage | Owns | Primary source |
|---|---|---|---|
| 01 | [parser](pipeline/01_parser/done.md) | Grammar, the DECIDE keyword tokens (`WHEN`, `PER`, `BY`), the transformer, the refusal of removed spellings | `third_party/libpg_query/grammar/statements/select.y`, `src/parser/decide/decide_removed_spellings.cpp` |
| 02 | [binder](pipeline/02_binder/done.md) | Decision declarations and their keys, where `when` / `per` / `by` may stand, types, degree, reducers, DECIDE validity | `src/planner/expression_binder/decide/*.cpp` |
| 03 | [logical_plan](pipeline/03_logical_plan/done.md) | `LogicalDecide`, subquery provenance, `AddConstraint` / `SetObjective`, serialization | `src/planner/operator/decide/logical_decide.cpp`, `src/planner/binder/query_node/plan_select_node.cpp` |
| 04 | [canonicalizer](pipeline/04_canonicalizer/done.md) | The one shape boundary: decisions left, bound right, the per-row / aggregate / value shape, the scope checks | `src/planner/decide/decide_canonicalizer.cpp` |
| 05 | [optimizer](pipeline/05_optimizer/done.md) | Formulation choice: NORM, IN, ABS, MIN/MAX, AVG, `<>`, bilinear, the value rewrite; direct solve's entry point | `src/optimizer/decide/decide_optimizer.cpp` |
| 06 | [model_formulation](pipeline/06_model_formulation/done.md) | `SolverInput` to `SolverModel`: variable layout, coefficients, bounds, Q matrix | `src/decidb/formulation/ilp_model_builder.cpp` |
| 07 | [solver](pipeline/07_solver/done.md) | Backend dispatch, Gurobi and HiGHS translation, status normalization, time limits | `src/decidb/solver/ilp_solver.cpp`, `src/decidb/gurobi/`, `src/decidb/naive/` |
| 08 | [execution](pipeline/08_execution/done.md) | Materialization, scope checks, coefficient evaluation, keyed-decision mapping, readback, slow-solve reports | `src/execution/operator/decide/physical_decide.cpp` |

## Reading order

1. [language/guide.md](language/guide.md): what you can write, with examples.
2. [language/scopes.md](language/scopes.md): how `per`, `when` and `by` decide what a clause generates.
3. [pipeline/architecture.md](pipeline/architecture.md): how the extension attaches to DuckDB.
4. [pipeline/trace_life_of_a_query.md](pipeline/trace_life_of_a_query.md): one query through all eight stages.
5. A stage's `done.md` as needed; [pipeline/code_structure.md](pipeline/code_structure.md) when you are about to edit
   source.

## Where a fact lives

Each fact has one home. Other documents link to it instead of restating it.

- **Syntax** is in [language/guide.md](language/guide.md); refusals and removed spellings in
  [language/errors.md](language/errors.md). Other documents cite them by section.
- **What a construct means** is in `language/`: [scopes.md](language/scopes.md),
  [functions.md](language/functions.md), [problem_types.md](language/problem_types.md). **How a stage implements it**
  is in that stage's `done.md`.
- **Why the language works as it does** is in [language/decisions.md](language/decisions.md); code and tests cite it by
  number (D1, S3).
- **Constraint and objective shape** (where a term may sit) is in
  [pipeline/04_canonicalizer/done.md](pipeline/04_canonicalizer/done.md) only.
- **Open work** is in the `todo.md` of the folder that owns the behavior. A defect no single stage owns is in
  [issues/bugs.md](issues/bugs.md); a code-quality problem in [issues/code_quality.md](issues/code_quality.md). A
  `todo.md` links to an issue rather than restating it.
- **Source is the truth.** When a document disagrees with the code, fix the document. When the behavior itself looks
  wrong, add it to `issues/` rather than documenting it as intended.

## Keeping these docs

- **Describe the system as it is now, and why.** No history: no "was", "used to", "fixed on", "after the merge", batch
  names or build logs. Git holds the history. A measurement or verification may carry the date it was taken.
- **`done.md` and `todo.md` are disjoint.** `done.md` says how something works today, in present tense. `todo.md` holds
  only open work: the problem, the decision it needs, the test that proves it, and the `done.md` section to update.
  When an item is finished, it leaves `todo.md` and its content becomes present-tense text in `done.md`, in the same
  change.
- **Never drop an open item.** When a `todo.md` is merged or deleted, every open item moves to the folder that owns it.
- **A folder keeps only files with content.** Do not add an empty `todo.md` or a stub `README.md`; this file is the only
  map, so a folder README exists only when the folder needs its own index.
- **The move to the deck is roadmap, not bugs.** A deck feature not built yet, and a current behavior the deck
  changes, go in [language/roadmap.md](language/roadmap.md), not in a `todo.md` or `issues/`. A bug is a wrong answer,
  a crash or a misleading message under today's documented language. Work that catches another part of DeciDB up to
  the new language (direct solve reading a `by` key) is a `todo.md` item, labelled as transition work.
- **Write plainly.** Short sentences, one idea each. Name the file, function or test. Use the standard name for a
  standard idea and add new terms to [glossary.md](glossary.md).
- **Links are relative** and point to a file in this tree; a section reference names the heading (`guide.md` §6.4).
