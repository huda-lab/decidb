# S1 (top-k and cardinality intervals) — what works today

Verified on 2026-10-05 at `8c9420edb2`: `make decide-test` (1,957 passed, also with `DECIDB_TEST_DIRECT_SOLVE=off` and
`DECIDB_VERIFY_SERIALIZER=1`), `DECIDB_FORCE_SOLVER=highs make decide-test` (1,956 passed, 1 unrelated failure that needs
Gurobi), and `build/release/test/unittest "[decidb]"` (907 assertions). Later commits touch only a comment and docs.
Code: `src/optimizer/decide/direct/s1_rule.cpp`. Class definition: [definition.md](definition.md).

## What it admits

One row-scoped `BOOL` decision `x` (output is `INTEGER` 0/1), with:

- **Count bounds on `SUM(x)`:** `<=`, `<`, `>=`, `>`, `=`, or several combined into one interval. A bound can be a constant,
  a foldable constant expression, or a numeric source column or source-only expression that varies per row (including
  `COALESCE` and `TRY_CAST`). Values are converted to DOUBLE as the solver does; strict and fractional limits become
  inclusive integer counts; limits are exact up to 2^53.
- **Bounds that count rows:** a bound built from `COUNT(*)`, constants and `+ - * / //` (`SUM(x) <= COUNT(*) / 2`,
  `>= COUNT(*) * 0.1`, `<= COUNT(*) - 1`, `-COUNT(*) + 6`), read per group. `COUNT(*)` counts the rows the clause covers in
  each group: a clause-level `WHEN` or a NULL `PER` key narrows it, while an aggregate-local `WHEN` on the left
  (`SUM(x) WHEN active <= COUNT(*) / 2`) does not narrow the right, which counts every row of the group. The expression is
  evaluated in its own types (a bound of `COUNT(*) * 0.57` over 100 rows is exactly 57) and then read as any source bound.
  A divisor must be a nonzero constant, and the proof bounds every node's value inside its own type, taking a group to have
  at most 2^53 rows (the limit counts already have; that many rows would need petabytes of memory).
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
  Coefficients are numeric, decision-free, deterministic; several terms must be non-throwing. Each `SUM(...)` may carry a
  constant factor or divisor (`2 * SUM(score*x)`, `SUM(score*x) / 2`, `-1.5 * SUM(...)`, `(1 + 1) * SUM(...)`) that scales
  that part's coefficients: a negative one reverses the part, zero makes every row score 0, and each part keeps its own.
  The factor must be a finite foldable constant, and a divisor must not be zero.

It raises an error wherever the solver does: empty scoped aggregate, invalid or NULL bounds (including on bypassed rows),
NULL or non-finite scores, infeasible counts. Every input row is read, so a late bad value raises under `LIMIT 1` or
`COUNT(*)`. When a query has several errors, direct may report a different one than the solver. When scores tie it may pick
different rows than the solver, with the same objective.

**Known difference, decided 2026-10-05: values within the solver's tolerance of an integer.** Direct solve reads every
bound exactly: counts, constants and column values. The solver accepts a violation of about 1e-6, and the two backends
differ from each other, so there is no single behavior to copy. `SUM(x) <= 1.9999999` selects 2 rows on the solver and 1 on
direct; `x >= 0.000001 WHEN id = 2` fixes the row on Gurobi but not on HiGHS, and `x <= 0.999999` fails inside HiGHS. A
column value computed in floating point (`0.1 * 10`) can land just under 1 and be read as below 1 by direct and as 1 by the
solver. Chosen over declining near-integer bounds because a column value is only known at run time, where the plan cannot
decline it. To avoid the difference, round the bound. `test_s1_direct_reads_bounds_exactly_near_an_integer` pins the
behavior, so changing it is deliberate.

**Error wording that differs from the solver** (on purpose; `../../architecture/policy.md`). A NULL or NaN bound column
raises `DECIDE: column "cap" has an invalid value (NULL or NaN). Impute it with COALESCE(cap, 0), or filter those rows out
with a WHERE clause.` for every column type; the solver words it by type and adds `at row N`. A NULL score quotes the score
expression (`DECIDE: score is NULL. Impute it with COALESCE(), ...`); the solver names the NULL columns of a computed
score. The empty-aggregate, non-finite score and infeasible messages are the solver's. Both paths raise on the same inputs.

## What it does not admit (falls back to the solver)

A miss never changes the answer: the query runs on the solver and `require` names the clause.

**Not yet admitted, planned** (`todo.md`):

| Shape | Task |
|---|---|
| Other aggregates on the right of a count bound: `SUM(x) <= AVG(cap)`, `SUM(col)`, `MIN`, `MAX`, `COUNT(col)`, and a right-hand aggregate with its own `WHEN` (`SUM(x) <= COUNT(*) WHEN active`, where the `WHEN` scopes only the count and the left side still sums every row) | S1-08 |

**Explicitly out of scope** (decided 2026-10-05; these stay on the solver on purpose):

| Shape | Why it is out | How to reopen it |
|---|---|---|
| `SUM(x + c)`, `SUM(1 - x)` (an offset or negated count body; was S1-03) | The definition says each selected item contributes exactly 1 to the count. A constant term makes the bound depend on the group size (`count + n*c <= B`), and `1 - x` flips the direction. The user can write the same limit as `SUM(x) >= n - k`. Bodies whose terms add up to exactly one `x` (`SUM(1*x)`, `SUM(2*x - x)`) are admitted | Admit `a*x + c` with `a = ±1` and a constant `c`: shift the per-group limit by `n*c` and flip the side when `a = -1`. About 0.5 day. Worth doing if users ask for "at most k rejected" |
| A bound expression that can raise at runtime, such as a narrowing `CAST` (was S1-02) | The solver raises on every row, including rows a `WHEN` or NULL `PER` key excludes, and direct solve must raise on the same inputs. Admitting it means proving DuckDB does not skip the check on a row the solver reads. Nonthrowing forms (`COALESCE`, `TRY_CAST`) are admitted, so `TRY_CAST` is the workaround. A multi-term objective whose coefficient can throw misses for the same reason | Prove the validation stage evaluates every row, then admit expressions that can only raise a cast or overflow error. Low value and costly |
| A factor on `SUM(...)` in the objective that is not a usable constant: a zero divisor, a NULL factor, or a subquery factor (`(SELECT 2) * SUM(...)`) | A zero divisor raises on the solver, which a plan built from a constant cannot reproduce. The solver reads a NULL factor as an objective of zeros, which is not a result worth copying. A subquery factor has no sign when the plan is built, and the sign decides whether the sense reverses | Admit a subquery factor once its value can be read before the plan is chosen. Low value |
| No objective (constraints only) | The definition requires a linear objective, and few queries are written this way | Treat every score as 0 so the plan picks only the rows a lower bound requires. About 1 hour: accept the missing objective in `Match` |
| A bound on `x` with no `WHEN` that the solver reads as a bound on the variable, not as Boolean arithmetic: a lower bound below 0 (`x >= -3`, `x > -3`, `x = -1`), a strict bound against a fraction (`x > 0.5`, `x < 1.5`), an upper bound below 0 or a lower bound above 1 (`x >= 2`, `x <= -1`) | A negative lower bound makes `x` signed, so the solver's result holds -3, -1 or -2 where direct would give 0 or 1. A strict bound against a fraction moves by a whole unit (`x > 0.5` becomes `x >= 1.5`), which gives an error or the wrong answer. A bound that cannot hold raises a message naming the clause, not DECIDE's infeasible error. Direct solve cannot match all three without copying the quirks. The same bound with a `WHEN` is plain arithmetic on the solver and is admitted | Fix the solver to treat these as Boolean arithmetic, then admit them. Not a direct-solve change |

**Belongs to another problem class or is intentional:** weighted constraints such as `SUM(w * x) <= B` (S3 and S4 cover two special cases of them); `INT` and
`REAL` variables (A1 and the R classes); more than one decision, entity or scalar decisions (D1); lower and upper bounds with
different `PER` or `WHEN` membership (S2); `SUM(x) <> k` (not an interval); bounds that are infinite, NaN or above 2^53
(not exactly representable); objective `PER` or `WHEN`, other reducers (`MIN`, `MAX`, `AVG`), `norm`; `DIAGNOSE`.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_three_way.py` (245 tests) | The answers. Each case runs on the independent ILP oracle (`oracle_solver`, built from the raw rows), on the solver path (`off`) and on the direct path (`require`, so a miss fails); both DeciDB runs must satisfy every constraint and reach the oracle's objective, or all must say infeasible. 170 cases: every comparison against whole and fractional limits (strict bounds as the solver path reads them), intervals, `PER` and NULL keys, top and aggregate-local `WHEN`, source-valued bounds and `COALESCE`, several bounds at once, row-count bounds (`COUNT(*)`, exact decimal arithmetic), pins and per-row bounds on `x` (a generated grid with and without `WHEN`), column pins, 11 objective shapes (sums, differences, negation, scaled and divided, offset, products), the nine beyond the plain score in both senses, tied scores, and infeasible problems. 10 parent-query contexts (CTE, wide source, joined and correlated source, parent join, recombined filters), a nested `DECIDE`, and empty inputs. 23 error cases that must fail on both paths (bad scores, empty aggregates, bad bounds on rows the problem does not read, late rows under `LIMIT` and `COUNT`, pins). 27 boundary cases against the solver path only (infinite and beyond-2^53 limits, every numeric source type, exact decimal arithmetic) and tiny scores against the exact finite-DOUBLE optimum |
| `test_direct_rule_contract.py` (73 tests) | The checks every rule owes: schema and rows, all-rows read, serializer, prepared plans, `EXPLAIN` and profiling, `off`, forced backend, `DIAGNOSE`, and 64 near misses, each of which must name its reason, keep the solver plan, and answer under `auto` as under `off` (this is where the shapes S1 leaves on the solver are tested) |
| `test_direct_fuzz.py` (8 tests) | Seeded differential fuzz: random small S1 queries, direct (`require`) against solver (`off`), over per-row bounds, scaled objectives and row-count bounds. Finds interactions the tables do not list |
| `test_direct_user_facing.py` (10 tests) | What a SQL user sees besides the answer: direct solve on by default, the pinned exact reading of a bound near an integer (`S1-09`), error wording that names the column, unused wide columns pruned, forced-backend and binder-error policy |
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
