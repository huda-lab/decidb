# S1 (top-k and cardinality intervals) — what works today

Verified on 2026-10-05 on the working tree after `c86298c9e8`, with the per-row bound change not yet committed: `make decide-test` (1,924
passed, also with `DECIDB_TEST_DIRECT_SOLVE=off` and `DECIDB_VERIFY_SERIALIZER=1`), `DECIDB_FORCE_SOLVER=highs make
decide-test` (1,923 passed, 1 unrelated failure that needs Gurobi), and `build/release/test/unittest "[decidb]"` (907
assertions). Re-stamp with the commit hash when it lands.
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
- **Per-row bounds on `x`:** any comparison (`=`, `<>`, `<`, `<=`, `>`, `>=`) against a foldable constant, a numeric source
  column, or a deterministic nonthrowing numeric expression over source columns (`COALESCE`, `TRY_CAST` of a column), with
  an optional source-only `WHEN`. Each row's value is read as the set of {0, 1} it allows. Both allowed means no
  constraint (`x <= 1`, `x >= 0`, `x BETWEEN 0 AND 1`, `x <> 2`); one allowed fixes the row to that value (`x >= 0.5`
  fixes to 1); none allowed makes the row infeasible. A bound with a `WHEN` takes any constant. A constant bound without a
  `WHEN` takes only the spellings where the solver's own reading matches Boolean arithmetic (see the out-of-scope table);
  a column value is read as arithmetic with or without a `WHEN`, as the solver does. A source Boolean column is also a pin
  value (`x = flag`). Contradictory active bounds are infeasible. A NULL or NaN column value raises on every row,
  including rows the `WHEN` excludes, and names the column.
- **Objective:** a signed sum of per-row coefficients times `x`, `MAXIMIZE` or `MINIMIZE`, plus an optional finite constant.
  Coefficients are numeric, decision-free, deterministic; several terms must be non-throwing.

It matches the solver on errors and order: empty scoped aggregate, invalid or NULL bounds (including on bypassed rows), NULL
or non-finite scores, infeasible counts. Every input row is read, so a late bad value raises under `LIMIT 1` or `COUNT(*)`.
When scores tie it may pick different rows than the solver, with the same objective.

**Known difference, decided 2026-10-05: values within the solver's tolerance of an integer.** Direct solve reads every
bound exactly: counts, constants and column values. The solver accepts a violation of about 1e-6, and the two backends
differ from each other, so there is no single behavior to copy. `SUM(x) <= 1.9999999` selects 2 rows on the solver and 1 on
direct; `x >= 0.000001 WHEN id = 2` fixes the row on Gurobi but not on HiGHS, and `x <= 0.999999` fails inside HiGHS. A
column value computed in floating point (`0.1 * 10`) can land just under 1 and be read as below 1 by direct and as 1 by the
solver. Chosen over declining near-integer bounds because a column value is only known at run time, where the plan cannot
decline it. To avoid the difference, round the bound. `test_s1_direct_reads_bounds_exactly_near_an_integer` pins the
behavior, so changing it is deliberate.

**Error wording that differs from the solver.** A NaN column value raises `DECIDE: column "pin" is NULL or NaN. Impute NULLs
with COALESCE(pin, 0) or filter those rows out with a WHERE clause.`; the solver raises its general right-hand-side message
with `at row N`, which direct does not copy (see `H-05` in `../_harness/todo.md`). A NULL in a floating point column reads
`is NULL or NaN` on direct and `is NULL` on the solver. Both paths raise on the same inputs.

## What it does not admit (falls back to the solver)

A miss never changes the answer: the query runs on the solver and `require` names the clause.

**Not yet admitted, planned** (`todo.md`):

| Shape | Task |
|---|---|
| Scaled objective terms: `2 * SUM(score*x)`, `SUM(score*x) / 2` | S1-06 |
| Bounds that count rows: `SUM(x) <= COUNT(*) / 2` | S1-07 |

**Explicitly out of scope** (decided 2026-10-05; these stay on the solver on purpose):

| Shape | Why it is out | How to reopen it |
|---|---|---|
| `SUM(x + c)`, `SUM(1 - x)` (an offset or negated count body; was S1-03) | The definition says each selected item contributes exactly 1 to the count. A constant term makes the bound depend on the group size (`count + n*c <= B`), and `1 - x` flips the direction. The user can write the same limit as `SUM(x) >= n - k`. Bodies whose terms add up to exactly one `x` (`SUM(1*x)`, `SUM(2*x - x)`) are admitted | Admit `a*x + c` with `a = ±1` and a constant `c`: shift the per-group limit by `n*c` and flip the side when `a = -1`. About 0.5 day. Worth doing if users ask for "at most k rejected" |
| A bound expression that can raise at runtime, such as a narrowing `CAST` (was S1-02) | The solver raises on every row, including rows a `WHEN` or NULL `PER` key excludes, and the error must come in the solver's order after the empty-aggregate error. Admitting it means proving DuckDB neither skips nor reorders the check. Nonthrowing forms (`COALESCE`, `TRY_CAST`) are admitted, so `TRY_CAST` is the workaround. A multi-term objective whose coefficient can throw misses for the same reason | Prove the evaluation order of the validation stage, then admit expressions that can only raise a cast or overflow error. Low value and costly |
| No objective (constraints only) | The definition requires a linear objective, and few queries are written this way | Treat every score as 0 so the plan picks only the rows a lower bound requires. About 1 hour: accept the missing objective in `Match` |
| A bound on `x` with no `WHEN` that the solver reads as a bound on the variable, not as Boolean arithmetic: a lower bound below 0 (`x >= -3`, `x > -3`, `x = -1`), a strict bound against a fraction (`x > 0.5`, `x < 1.5`), an upper bound below 0 or a lower bound above 1 (`x >= 2`, `x <= -1`) | A negative lower bound makes `x` signed, so the solver's result holds -3, -1 or -2 where direct would give 0 or 1. A strict bound against a fraction moves by a whole unit (`x > 0.5` becomes `x >= 1.5`), which gives an error or the wrong answer. A bound that cannot hold raises a message naming the clause, not DECIDE's infeasible error. Direct solve cannot match all three without copying the quirks. The same bound with a `WHEN` is plain arithmetic on the solver and is admitted | Fix the solver to treat these as Boolean arithmetic, then admit them. Not a direct-solve change |

**Belongs to another problem class or is intentional:** weighted constraints such as `SUM(w * x) <= B` (S3 and S4 cover two special cases of them); `INT` and
`REAL` variables (A1 and the R classes); more than one decision, entity or scalar decisions (D1); lower and upper bounds with
different `PER` or `WHEN` membership (S2); `SUM(x) <> k` (not an interval); bounds that are infinite, NaN or above 2^53
(not exactly representable); objective `PER` or `WHEN`, other reducers (`MIN`, `MAX`, `AVG`), `norm`; `DIAGNOSE`.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_solve.py` (237 tests) | Path selection, independent enumeration of small optima, exact bounds and strict/fractional normalization, `PER`/`WHEN`/NULL-key cases, pins, signed objectives, near misses with reasons, parent and CTE contexts, late errors, wide-output pruning, direct vs both solvers. Per-row bounds: every comparison against eight constants, with and without a `WHEN`, direct against the solver (and the exact list of spellings that stay on the solver); numeric column pins against the solver for every comparison, NULL, NaN and infinite values, and per-row and per-group variation; the out-of-scope shapes still answer on the solver |
| `test_direct_solve_oracle.py` (33 tests) | Direct results against the independent ILP oracle (`oracle_solver`), including per-row bounds with fractions and out-of-domain constants, numeric column pins and impossible bounds, and 8 seeded differential fuzz tests (direct under `require` vs solver under `off`) that also generate per-row constant and column bounds |
| `test_direct_rule_contract.py` (33 tests) | The checks every rule owes: schema and rows, all-rows read, serializer, prepared plans, `EXPLAIN` and profiling, near misses, `off`, forced backend, `DIAGNOSE` |
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
