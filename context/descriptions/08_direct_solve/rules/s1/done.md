# S1 (top-k and cardinality intervals) — what works today

Verified at `a1ac47407e` on 2026-10-05 with `make decide-test` (1,883 passed, also with `DECIDB_TEST_DIRECT_SOLVE=off` and
`DECIDB_VERIFY_SERIALIZER=1`), `DECIDB_FORCE_SOLVER=highs make decide-test` (1,882 passed, 1 unrelated failure that needs
Gurobi), and `build/release/test/unittest "[decidb]"` (907 assertions).
Code: `src/optimizer/decide/direct/s1_rule.cpp`. Class definition: [definition.md](definition.md).

## What it admits

One row-scoped `BOOL` decision `x` (output is `INTEGER` 0/1), with:

- **Count bounds on `SUM(x)`:** `<=`, `<`, `>=`, `>`, `=`, or several combined into one interval. A bound can be a constant,
  a foldable constant expression, or a numeric source column or source-only expression that varies per row (including
  `COALESCE` and `TRY_CAST`). Values are converted to DOUBLE as the solver does; strict and fractional limits become
  inclusive integer counts; limits are exact up to 2^53.
- **Count bodies:** `SUM(x)` or any body whose terms add up to exactly one `x`, such as `SUM(1*x)` or `SUM(2*x - x)`.
- **Scope:** global, or `PER` one or more source columns, optionally with a deterministic source-only `WHEN` (top level or
  inside the aggregate). All count clauses must share identical membership. Rows with a NULL `PER` key or a false aggregate
  `WHEN` bypass the bound but still obey per-row pins.
- **Pins:** per-row `x = 0`, `x <= 0`, `x < 1`, `x = 1`, `x >= 1`, `x > 0` with an optional source-only `WHEN`. Contradictory
  active pins are infeasible.
- **Objective:** a signed sum of per-row coefficients times `x`, `MAXIMIZE` or `MINIMIZE`, plus an optional finite constant.
  Coefficients are numeric, decision-free, deterministic; several terms must be non-throwing.

It matches the solver on errors and order: empty scoped aggregate, invalid or NULL bounds (including on bypassed rows), NULL
or non-finite scores, infeasible counts. Every input row is read, so a late bad value raises under `LIMIT 1` or `COUNT(*)`.
When scores tie it may pick different rows than the solver, with the same objective.

## What it does not admit (falls back to the solver)

Weighted constraints such as `SUM(w * x) <= B`; `INT` and `REAL` variables; more than one decision; entity or scalar
decisions; objective `PER` or `WHEN`; other reducers (`MIN`, `MAX`, `AVG`); `norm`; `DIAGNOSE`; throwing source expressions as
bounds; numeric source-valued per-row pins; offset count bodies such as `SUM(x + 0)`. Open work: `todo.md`.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_solve.py` (209 tests) | Path selection, independent enumeration of small optima, exact bounds and strict/fractional normalization, `PER`/`WHEN`/NULL-key cases, pins, signed objectives, near misses with reasons, parent and CTE contexts, late errors, wide-output pruning, direct vs both solvers |
| `test_direct_solve_oracle.py` (22 tests) | Direct results against the independent ILP oracle (`oracle_solver`) and 8 seeded differential fuzz tests (direct under `require` vs solver under `off`) |
| `test_direct_rule_contract.py` (31 tests) | The checks every rule owes: schema and rows, all-rows read, serializer, prepared plans, `EXPLAIN` and profiling, near misses, `off`, forced backend, `DIAGNOSE` |
| Tiny-score fixture | Direct against the exact finite-DOUBLE optimum from 5e-324 to 1e-6. The two solvers disagree with each other near 1e-9, so backend comparisons allow a measured gap of 1e-7 |
| C++ `[decidb]` | S1 proof contract, facts, coordinator |

## Performance

Measured 2026-10-05 at `7d6f97e880`, before the NULL-score message change, and not re-run since (macOS arm64, 11 cores). 5,000,000 stored rows; every query ran in a fresh process
that only opens the database file, so peak memory is the query's own. Times are median query-plus-collection seconds
(direct 2 to 4 runs, Gurobi 1 or 2). In all 50 runs direct and Gurobi agreed on row count, selected count and objective.
HiGHS was not re-run; earlier it took about 43 s on the global shape.

| Shape | Direct | Gurobi | Speedup | Direct peak | Gurobi peak |
|---|---:|---:|---:|---:|---:|
| Global top-K, narrow, full output | 0.20 s | 2.85 s | 14x | 1.0 GiB | 1.7 to 2.3 GiB |
| Global top-K, narrow, aggregate output | 0.17 s | 2.72 s | 16x | 0.9 GiB | 2.2 GiB |
| Global top-K, unused 512 B payload (pruned) | 0.16 s | 3.17 s | 20x | 0.8 GiB | 3.6 GiB |
| 100 `PER` groups, fixed pins | 0.32 s | 33.7 s | 104x | 0.5 GiB | 3.7 GiB |
| 100 groups, aggregate-local `WHEN` | 0.30 s | 28.3 s | 95x | 0.5 GiB | 3.4 GiB |
| 100 groups, local `WHEN`, source-valued bound | 0.55 s | 27.8 s | 50x | 0.7 GiB | 3.4 GiB |
| 100 groups, source-valued bounds | 0.51 s | 30.8 s | 61x | 0.7 GiB | 4.0 GiB |
| 100 groups, paired source-valued bounds | 0.65 s | 31.3 s | 48x | 0.8 GiB | 3.8 GiB |
| 100 groups, `COALESCE` source-valued bound | 0.58 s | 35.4 s | 62x | 0.7 GiB | 2.5 GiB |

The speedup comes mostly from the solver being slow here: it builds a model with millions of variables, which direct solve
skips. A shape whose solver model is cheap would show a smaller win.

**Weak spots**

| Case | Result |
|---|---|
| Wide rows (512 B) returned in full | Direct 1.1 to 2.0 s (two runs: 1.99 s then 1.08 s) versus Gurobi 3.5 to 3.7 s, so 1.8 to 3.4x. Direct's plan itself takes about 0.4 to 0.5 s; the rest is result collection, roughly 0.6 to 1.5 s against about 0.5 s for Gurobi. Memory 2.8 to 3.6 GiB versus 3.8 to 4.3 GiB. Cause not isolated: `S1-05` in `todo.md` |
| Small inputs, up to 10,000 rows | Everything finishes in about 20 ms, so no real difference (earlier measurement, not re-run) |
| 100,000 wide rows, trivial capacity | Direct about 5% slower than Gurobi, two runs (earlier measurement, not re-run). The only measured loss |
| Capacity 0 or all rows, 5M narrow | Still wins, by 3 to 4x instead of 16x (earlier measurement, not re-run) |

**Reproduce** (after `make release`; the sweep above took about 4.5 minutes):

```sh
clang++ -std=c++17 -O2 -I src/include benchmark/decide/profile_direct_s1_api.cpp \
  -L build/release/src -lduckdb -Wl,-rpath,$PWD/build/release/src \
  -o benchmark/decide/results/profile_direct_s1_api
P="python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --db-dir /tmp/s1db --keep-db --timeout 180"
$P --widths 0 512 --output-kinds full consume --modes direct gurobi --repeats 2
$P --widths 512 --output-kinds aggregate --modes direct gurobi --scopes global --cardinalities upper --repeats 2
$P --widths 0 --output-kinds aggregate --modes direct gurobi --scopes grouped_local_when --cardinalities interval --bound-kinds constant source --repeats 1
$P --widths 0 --output-kinds aggregate --modes direct gurobi --scopes grouped --pins fixed --cardinalities interval --repeats 1
$P --widths 0 --output-kinds aggregate --modes direct gurobi --scopes grouped --cardinalities interval --bound-kinds source source_pair source_coalesce --repeats 1
```

Add `--output file.csv` to keep the raw rows. The runner pins `require` for direct and `off` for the solver, so a run never
silently uses the other path. The wide database needs about 3 GiB of disk.
