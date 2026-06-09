# DeciDB

DeciDB extends DuckDB with a DECIDE clause for in-database Integer Linear Programming.
See `context/descriptions/` for full documentation (start with `README.md` there).

## Build

```bash
make release                         # Linux release build
```

Build output: `build/release/decidb` (CLI), `build/release/src/libduckdb.so`

## Test

```bash
make decide-test                       # Run DECIDE differential tests
make decide-setup                      # Setup test environment only
```
Tests are in `test/decide/`.

## Benchmark

```bash
make decide-bench-setup                # Generate TPC-H databases (small/medium/large)
make decide-bench                      # Run all queries × all sizes (with stage timers)
make decide-bench-manual               # Run custom query from queries/manual.sql
make decide-view                       # View latest results (colored stage bars)
```

Selective: `python3 benchmark/decide/run_benchmarks.py --sizes small --queries Q1,Q3 --compare`

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

## DECIDE Syntax (Quick Reference)

```sql
SELECT select_list
FROM table_expression
[WHERE ...]
DECIDE [Table.]variable_name [IS type] [, ...]
SUCH THAT constraint [AND constraint ...]
[MAXIMIZE | MINIMIZE] SUM|MIN|MAX(linear_expression)
[MINIMIZE] SUM(POWER(linear_expression, 2))  -- convex QP
[MAXIMIZE] SUM(-POWER(linear_expression, 2)) -- concave QP (both solvers)
[MAXIMIZE] SUM(POWER(linear_expression, 2))  -- non-convex QP (Gurobi only)
[MAXIMIZE|MINIMIZE] SUM(bool_var * other_var) -- bilinear, McCormick (both solvers)
[MAXIMIZE|MINIMIZE] SUM(var_a * var_b)        -- bilinear, non-convex (Gurobi only)
```

- Variable types: `IS BOOLEAN` (0/1), `IS INTEGER` (default, non-negative), `IS REAL` (continuous, non-negative)
- **Table-scoped variables**: `DECIDE Table.var IS TYPE` — one variable per unique entity in the source table instead of one per result row. All result rows from the same entity share the same variable value. Reduces solver variable count from `num_rows` to `num_entities`. Table qualifier must match an alias or table name in the FROM clause. Mixed queries can have both row-scoped and table-scoped variables.
- Constraints: linear expressions with `=`, `<`, `<=`, `>`, `>=`, `<>`, `BETWEEN`, `IN` (all operators supported on both per-row and aggregate constraints; `IN` on aggregates not supported)
- Conditional: `expression WHEN condition` (postfix, on constraints and objectives)
- Grouping: `SUM(expr) op rhs PER column` or `PER (col1, col2, ...)` (one constraint per distinct value/combination). Empty groups (WHEN filters out all rows in a group) are skipped.
- `SUM()` aggregate supported over decision variables; `AVG(expr)` supported (rewritten to SUM with RHS scaling by row count at execution time)
- `MIN(expr)` / `MAX(expr)` supported in constraints and objectives via linearization:
  - **Easy cases** (no Big-M): `MAX(expr) <= K` → per-row `expr <= K`; `MIN(expr) >= K` → per-row `expr >= K`. The aggregate is simply stripped because bounding every row individually is equivalent.
  - **Hard cases** (Big-M indicators): `MAX(expr) >= K`, `MIN(expr) <= K`, equality. These require a binary indicator variable per row because "at least one row satisfies" is a disjunctive constraint that can't be expressed linearly without Big-M.
  - **Easy objectives**: `MINIMIZE MAX(expr)`, `MAXIMIZE MIN(expr)` → global auxiliary variable `z` with per-row linking constraints (`z >= expr_i` or `z <= expr_i`).
  - **Hard objectives**: `MAXIMIZE MAX(expr)`, `MINIMIZE MIN(expr)` → global `z` + per-row binary indicators + `SUM(y) >= 1`, because finding the row that achieves the optimum requires indicator selection.
- **PER on objectives**: Nested aggregate syntax `OUTER(INNER(expr)) PER col` where OUTER/INNER ∈ {SUM, MIN, MAX, AVG}. AVG as outer maps to SUM (constant divisor). AVG as inner scales coefficients by `1/n_g` (group size) — NOT equivalent to SUM when groups have different sizes. Flat `SUM/AVG + PER` is a no-op; flat `MIN/MAX + PER` is an error (ambiguous). Two-level ILP formulation: inner creates per-group auxiliaries, outer creates global auxiliary. Easy/hard classification applies at each level independently.
- **Quadratic objectives (QP)**: Three syntax forms: `POWER(expr, 2)`, `expr ** 2`, `(expr) * (expr)`. Negated forms also supported: `-POWER(expr, 2)`, `(-1) * POWER(expr, 2)`. MINIMIZE with PSD Q and MAXIMIZE with NSD Q are convex (both solvers). MAXIMIZE with PSD Q is non-convex (Gurobi only, via NonConvex=2). Gurobi supports MIQP; HiGHS supports continuous convex QP only. Linear terms may be mixed into the same objective as a quadratic term — `SUM(POWER(x - t, 2) + c * x)` and `SUM(POWER(x - t, 2)) + SUM(c * x)` are equivalent; linear coefficients populate the c vector while the POWER populates Q. Only one quadratic group (single POWER / self-product) per objective is supported; multiple groups (e.g., `SUM(POWER(x,2)) + SUM(POWER(y,2))`) are rejected.
- **Bilinear terms (`x * y`)**: Product of two different DECIDE variables supported in objectives and constraints. Each factor must be linear in decision variables — shapes like `x * POWER(y, 2)` (degree 3), `POWER(x, 2) * POWER(y, 2)` (degree 4), or `POWER(POWER(x, 2), 2)` are rejected with `InvalidInputException`; only total degree ≤ 2 is supported. Two categories:
  - **Boolean × anything** (linearizable): When one factor is Boolean, McCormick envelopes produce exact MILP reformulation. Works with both Gurobi and HiGHS. Requires a finite upper bound on the non-Boolean variable. Bool×Bool uses simpler AND-linearization (no Big-M).
  - **General (non-convex)**: Real×Real, Int×Int, Int×Real produce indefinite Q matrix (off-diagonal entries). Objectives: Gurobi only (NonConvex=2). Constraints: Gurobi only (via `GRBaddqconstr`). HiGHS rejects with clear error.
- Constraints support both linear and bilinear terms, plus quadratic self-product terms (e.g. `SUM(POWER(x, 2)) <= K`, Gurobi-only via `GRBaddqconstr`). Objectives can be linear, quadratic (POWER), bilinear, or mixed.

For full syntax details: `context/descriptions/00_project_overview/syntax_reference.md`
For keyword-by-keyword reference: `context/descriptions/03_expressivity/`

## Core Principles

- **Follow DuckDB patterns first**: When adding a feature, find how DuckDB handles the analogous SQL case and mirror that approach. Don't invent new patterns when DuckDB already has one.
- **Solver-agnostic**: Features must work with both Gurobi and HiGHS. Don't rely on solver-specific capabilities without a fallback path.
- **Minimal DuckDB core modifications**: DeciDB extends DuckDB; prefer adding new code over modifying core DuckDB files. The less we touch upstream, the easier version upgrades are.

## Demand Elegance (Balanced)

- For non-trivial changes: pause and ask "is there a more elegant way?" before presenting
- If a fix feels hacky: step back and implement the elegant solution
- Skip this for simple, obvious fixes — don't over-engineer

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

1. **Performance**: Use `/bench` for the optimize-measure loop. Solver dominates (~95% at scale). Focus on reducing solver input size and improving formulations.
   - Methodology: `context/descriptions/02_operations/benchmarking.md`
   - History of applied optimizations + outcomes: `context/descriptions/06_performance/`
2. **Expressivity**: See `context/descriptions/03_expressivity/` (each keyword has `done.md`/`todo.md`)
3. **Optimizer**: matrix efficiency, algebraic rewrites (Big-M reformulation, push-down / pull-out)
   - See `context/descriptions/04_optimizer/` (each strategy area has `done.md`/`todo.md`; start with its `README.md`)

## Documentation

Full docs in `context/descriptions/` — start with `README.md` there for navigation and reading order.
Key areas: `00_project_overview/` (syntax spec), `01_pipeline/` (architecture), `03_expressivity/` (feature status), `04_optimizer/` (rewrite strategies).

**MANDATORY: Keep docs in sync with code changes.** Whenever a code change affects the behavior, semantics, or implementation of a feature documented in `context/descriptions/`, you MUST update the relevant `done.md` (and `todo.md` if applicable) in the same work session. This includes:
- Semantic changes (how a feature works)
- Implementation changes (data structures, code paths, function signatures)
- Code Pointers sections (line numbers, file references, tag constants)
- New feature interactions (e.g., WHEN+PER composition)
- **Performance commits**: a perf-log entry in `context/descriptions/06_performance/` is added as a follow-up commit once the user has run the benchmark and shared numbers — see the dedicated section above. Do not block the code commit on writing the log.

If unsure which doc to update, check `context/descriptions/README.md` for the directory layout. Ask the user for confirmation if which doc to update is not clear or if a new doc may be needed.
