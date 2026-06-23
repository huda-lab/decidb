# DeciDB

DeciDB extends DuckDB with a DECIDE clause for in-database Constrained Optimization Problems.
See `context/descriptions/` for full documentation (start with `README.md` there).

## Build

```bash
make release                         # Release build
```

Build output: `build/release/decidb` (CLI), `build/release/src/libduckdb.dylib` (macOS) / `libduckdb.so` (Linux)

## Test

```bash
make decide-test                       # Run DECIDE differential tests
make decide-setup                      # Setup test environment only
```
Tests are in `test/decide/`.

## Benchmark

```bash
make decide-bench-setup                # Generate TPC-H databases (medium/large)
make decide-bench                      # Run all queries × all sizes (with stage timers)
make decide-bench-manual               # Run custom query from queries/manual.sql
make decide-view                       # View latest results (colored stage bars)
```

Selective: `python3 benchmark/decide/run_benchmarks.py --sizes medium --queries Q1,Q3 --compare`

**Optimization loop**: Use `/bench` to automate build → benchmark (medium) → analyze stages → suggest next optimization. Full docs: `context/descriptions/02_operations/benchmarking.md`.

**Recording performance commits.** When a commit lands whose primary purpose is improving performance, the user runs the benchmark and then asks for a perf-log entry to be appended to `context/descriptions/06_performance/` (one file per batch, named `{NNN}_{baseline_commit}_{evaluated_commit}.md` where `NNN` is the next sequential log number — e.g., `002_9c3a53fb62_6bc8ae1412.md`). The entry should reference the commit hash, list the change set + hypothesis, and include measured deltas vs. the prior baseline (with both `benchmark/decide/results/<commit>.json` paths). The log is append-only — supersede entries by adding a new one, don't rewrite history. The perf-log lands as a follow-up commit after the user has measured; do not write it speculatively at code-commit time.

## Key DeciDB source paths

- Parser/Symbolic: `src/decidb/symbolic/decide_symbolic.cpp`
- Binder: `src/planner/expression_binder/decide_binder.cpp`, `decide_constraints_binder.cpp`, `decide_objective_binder.cpp`
- Logical operator: `src/planner/operator/logical_decide.cpp`
- Optimizer: `src/optimizer/decide/decide_optimizer.cpp` (algebraic rewrites: AVG→SUM, ABS linearization, MIN/MAX classification, `<>` indicators, bilinear McCormick linearization)
- Physical execution + solver integration (Gurobi/HiGHS): `src/execution/operator/decide/physical_decide.cpp`
- Solver integration: `src/decidb/utility/ilp_model_builder.cpp` (SolverInput → SolverModel, VarIndexer, quadratic constraint emission), `src/decidb/utility/ilp_solver.cpp` (facade), `src/decidb/gurobi/gurobi_solver.cpp` (Gurobi backend), `src/decidb/naive/deterministic_naive.cpp` (HiGHS backend)
- Headers: `src/include/duckdb/` (see `common/enums/decide.hpp`, `planner/operator/logical_decide.hpp`, `optimizer/decide_optimizer.hpp`, `decidb/solver_input.hpp`, `decidb/ilp_model.hpp`, etc.)

## DECIDE Syntax

Full syntax specification (clause shapes, variable types, constraints, WHEN/PER, MIN/MAX linearization, QP, bilinear): `context/descriptions/00_project_overview/syntax_reference.md`
Keyword-by-keyword feature status: `context/descriptions/03_expressivity/` (each keyword has `done.md`/`todo.md`)

**Read the syntax reference before working on DECIDE semantics, formulations, or tests** — do not rely on recalled syntax.

## Core Principles

- **Follow DuckDB patterns first**: When adding a feature, find how DuckDB handles the analogous SQL case and mirror that approach. Don't invent new patterns when DuckDB already has one.
- **Solver-agnostic**: Features must work with both Gurobi and HiGHS. Don't rely on solver-specific capabilities without a fallback path.
- **Minimal DuckDB core modifications**: DeciDB extends DuckDB; prefer adding new code over modifying core DuckDB files. The less we touch upstream, the easier version upgrades are.

## Demand Elegance (Balanced)

- For non-trivial changes: pause and ask "is there a more elegant way?" before presenting
- If a fix feels hacky: step back and implement the elegant solution
- Skip this for simple, obvious fixes — don't over-engineer

## Issue Logging (Opportunistic)

While working on any task, if you spot a potential bug or code-quality issue along the way:

1. **Log it** in `context/descriptions/07_issues/`:
   - Bug (wrong results, crash, unsound formulation) → `bugs/todo.md`
   - Code quality (duplication, dead code, fragile pattern, unclear naming, missing tests) → `code_quality/todo.md`
   - Entry: short title, location (`file:line`), what's wrong, why it matters, date + task during which it was discovered.
2. **Triage by relevance to the current task**:
   - Could affect the current task (its correctness, results, or tests it depends on): flag it to the user, PAUSE the task, and wait for the user to resolve it and say continue.
   - Irrelevant to the current task: log the entry and keep working without interruption; mention it briefly in the final summary.

## Conventions

- DeciDB code follows DuckDB coding conventions (CamelCase classes, snake_case methods)
- Libraries are named `libduckdb.*` internally for DuckDB API compatibility
- The executable is named `decidb`
- DECIDE clause keywords: DECIDE, SUCH THAT, MAXIMIZE, MINIMIZE, WHEN
- WHEN is postfix on constraints and objectives: `expression WHEN condition` (not `WHEN condition THEN expression`)
- Constraints support linear and bilinear terms; objectives support linear, quadratic (QP via `POWER`), bilinear (`x * y`), or mixed forms
- Solver strategy: Gurobi (primary, commercial) — empirically much faster in practice. HiGHS (bundled, open-source) is retained as a fallback only; it is significantly slower and not recommended for production use.
- Always use `python3` (not `python`) — `python` is not available on this system

## Grammar Changes

Editing `third_party/libpg_query/grammar/` `.y`/`.yh` files requires regeneration before building:
```bash
make grammar-build                     # regenerate grammar + rebuild (one step)
make grammar                           # regenerate grammar only (requires bison 2.3)
```
The `.y` files are templates; the actual compiled parser is `third_party/libpg_query/src_backend_parser_gram.cpp` (generated).

## Lessons

See `.claude/lessons.md` for corrections and gotchas discovered during development. Update it after any mistake to prevent recurrence.

## Development Priorities

**Current focus: Query Diagnostics & Solver Behavior.** Turn failed or useless DECIDE solves (infeasible / unbounded / slow) into actionable, **least-change** diagnoses — why it failed and the smallest edit that restores a usable solution — and empirically characterize how Gurobi and HiGHS behave on these cases (status codes, unbounded rays, incumbents at timeout, time limits, interrupts) before building on them.
   - Area docs + build order: `context/descriptions/08_query_diagnostics/` — start with its `README.md` (the router decision tree is the area's single map). Architecture is **substrate → spine → terminals**: `foundations/` (substrate: shared plumbing + solver-behavior reference), `router/` (spine: the post-solve dispatch tree, disambiguates inf/unb by ray extraction), and the terminal engines `unbounded/` · `infeasible/` (flagship elastic) · `slow/`. Each folder has `done.md` (how it currently works) + `todo.md` (remaining work only — shipped items move to `done.md`).
   - Principles: **diagnosis on by default** (`PRAGMA diagnose_decide` is `auto` by default — a failed solve is diagnosed automatically wherever an engine exists; set `off` to suppress and get the plain static solver error. We still never edit the user's query) and **solver-agnostic** (build the elastic model in our own model builder so both backends run it; Gurobi-only APIs like `feasRelax` are accelerators, never dependencies).
   - Solver-behavior groundwork: probe both backends first — the items marked 🔬 in the `08_query_diagnostics/` todos (HiGHS time-limit/status/ray/warm-start, the interrupt mechanism) are empirical unknowns to resolve before implementing.

Ongoing (secondary):
- **Optimizer**: matrix efficiency, algebraic rewrites (Big-M reformulation, push-down / pull-out). `context/descriptions/04_optimizer/` (each strategy area has `done.md`/`todo.md`; start with its `README.md`).

## Documentation

Full docs in `context/descriptions/` — start with `README.md` there for navigation and reading order.
Key areas: `00_project_overview/` (syntax spec), `01_pipeline/` (architecture), `03_expressivity/` (feature status), `04_optimizer/` (rewrite strategies).

**MANDATORY: Keep docs in sync with code changes.** Whenever a code change affects the behavior, semantics, or implementation of a feature documented in `context/descriptions/`, you MUST update the relevant `done.md` (and `todo.md` if applicable) or `{description}.md` file in the same work session. This includes:
- Semantic changes (how a feature works)
- Implementation changes (data structures, code paths, function signatures)
- Code Pointers sections (line numbers, file references, tag constants)
- New feature interactions (e.g., WHEN+PER composition)
- **Performance commits**: a perf-log entry in `context/descriptions/06_performance/` is added as a follow-up commit once the user has run the benchmark and shared numbers — see the dedicated section above. Do not block the code commit on writing the log.

If unsure which doc to update, check `context/descriptions/README.md` for the directory layout. Ask the user for confirmation if which doc to update is not clear or if a new doc may be needed.
