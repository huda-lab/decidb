# S1 (uniform-matroid optimization) — what works today

Verified on 2026-10-27 at `a14b1f8129` or later, with the strict xfails on the direct path of pending cases: `make decide-test`,
`DECIDB_TEST_DIRECT_SOLVE=off make decide-test` and `DECIDB_VERIFY_SERIALIZER=1 make decide-test` each 4,521 passed
and 335 strict expected failures (the direct path of keyed and folded-affine cases, other direct-solve follow-ups
and the deferred bound reader); `DECIDB_FORCE_SOLVER=highs
make decide-test` 4,519 passed and two failures, `test_norm_combined_l1_l2_objective`, which needs Gurobi, and
`test_entity_scoped_mixed_when_per`, which HiGHS cannot close at a gap of 0 in time (`../../../issues/bugs.md`); C++
`build/release/test/unittest "[decidb]"` 2,714 assertions in 65 cases. `test/decide/run_direct_matrix.sh` renews these numbers in one command (it prints one line per check). Code:
`src/optimizer/decide/direct/s1_uniform_matroid_rule.cpp`; its scope projection and bound validation are the shared
fragments in `direct_plan_fragments.{hpp,cpp}`. Class definition: [definition.md](definition.md).

## What it admits

One row-scoped `BOOL` decision `x` (output is `INTEGER` 0/1), with:

- **Count bounds on `SUM(x)`:** `<=`, `<`, `>=`, `>`, `=`, or several combined into one interval. A bound can be a constant,
  a foldable constant expression, or a numeric source column or deterministic expression over source columns that varies per
  row, including one that can raise (`cap + 1`, a narrowing `CAST`). A data bound reaches S1 already reduced and checked
  by the bound-side window rewrite (`../../framework/done.md`): one value on every kept row (the MIN of an upper bound, the MAX
  of a lower one, and an `=` bound that must agree), with its NULL, NaN and disagreement errors raised by the scope
  checks under the plan, in the solver's words. S1 reads that value in its score projection, on the rows its clause
  keeps; nothing is computed on a row the `when` drops (syntax reference §6.3). Values are converted to DOUBLE as the
  solver does; strict and fractional limits become inclusive integer counts; limits are exact up to 2^53.
- **Bounds read from the kept rows:** `COUNT(*)`, `COUNT`, `MIN`, `MAX`, `SUM` and `AVG` of a numeric source expression on
  the right of a count bound, alone or inside `+ - * / //` and other arithmetic (`SUM(x) <= COUNT(*) / 2`,
  `>= COUNT(*) * 0.1`, `<= MIN(cap)`, `>= MAX(lo) / 2`, `<= SUM(cap)`), with or without a `when` of its own
  (`<= sum(when f: 1)`, `<= min(when cap > 1: cap)`), a written norm and a keyed or colon reducer. The bound-side window
  rewrite (`../../framework/done.md`) hands S1 each reducer as a source column holding its value, so S1 reads the bound like
  any data bound. S1 has no reader of its own for a reducer. The value is DuckDB's, in its own types: `SUM` over any numeric type is
  exact, a bound of `COUNT(*) * 0.57` over 100 rows is exactly 57, and over the `BIGINT` values 2^60, 1, -2^60
  `SUM(cap)` is 1. `COUNT(*)` counts the rows the clause's `when` keeps; a `when` inside the left-hand sum
  (`SUM(when active: x) <= COUNT(*) / 2`) does not narrow the right. An infinite value is a value. An overflow
  (`COUNT(*) * 4611686018427387904` over 3 rows) raises as on the solver path, and only on a row the clause keeps. The use
  case is a top-fraction query ("the best 10% of the rows": `SUM(x) <= COUNT(*) * 0.1`). A reducer with a `by` key makes
  S1 decline; reading one per group is open work (`todo.md`).
- **Count bodies:** `SUM(a*x)` where the terms in `x` add up to exactly `a = 1` or `a = -1` (`SUM(x)`, `SUM(1*x)`,
  `SUM(2*x - x)`, `SUM(-x)`); `SUM(-x) <op> B` is `SUM(x) <flipped op> -B`. A body with a constant term (`SUM(x + 1)`,
  `SUM(1 - x)`, `SUM(x + 0.5)`) declines, and the solver answers it. The class asks only for a count of `x`, and
  admitting a constant meant shifting the limit by `n*c` per group with an exactness cap and its own overflow cases.
- **Scope:** global, optionally with a deterministic source-only `when` in front of the clause, inside the sum, or
  both (the sum then counts the rows both keep). All count clauses must share identical membership. Rows the sum's own `when` leaves out are outside the count but still obey
  per-row fixed values. A `when` that keeps no row is an error (D4), and so is a sum whose own `when` keeps no row (D8):
  the plan raises both before it reads a value, as the solver does (`AddRowChecks` in `direct_rule_driver.cpp`). A `by`
  key on the sum, or a `per K:` in front of the clause, makes S1 decline (`todo.md`).
- **Per-row bounds on `x`:** any comparison (`=`, `<>`, `<`, `<=`, `>`, `>=`) against a foldable constant, a numeric
  source column, or a deterministic nonthrowing numeric expression over source columns (`COALESCE`, `TRY_CAST` of a
  column), with an optional source-only `when`. Each row's value is read as the set of {0, 1} it allows. Both allowed
  means no constraint (`x <= 1`, `x >= 0`, `x BETWEEN 0 AND 1`, `x <> 2`); one allowed fixes the row to that value (`x
  >= 0.5` fixes to 1); none allowed makes the row infeasible. A bound with a `when` takes any constant. A constant bound
  without a `when` is read the same way when it can hold: a lower bound below 0 (`x >= -3`, `x > -3`) allows both
  values, because the solver keeps a Boolean's box at [0, 1] and a written bound only narrows it. One that cannot hold
  is left to the solver (see the out-of-scope table). A bound on `x` allows the values in the interval the solver path's
  classifier admits (`DecideReadSingletonRow`, D64), whatever its constant coefficient: `x > 0.5` and `2*x >= 1`
  allow 1, `x < 1.5` allows 0 and 1, `2*x <= cap` allows 0 where `cap` is below 2. One expression says whether a row
  allows `x = v`, for 0 and for 1: folded once against a constant, evaluated per row against a column value, with or
  without a `when`, as the solver does. Whether a constant
  bound is one the solver folds into the variable's box is the IR's `variable_bound` flag (no `when`), the same flag A1
  reads. The rest of the reading stays S1's own and is not the shared bound reader's: S1 asks which of {0, 1} a row
  allows, A1 asks for an interval, and S1 alone admits `<>` and Boolean sources (`x = flag`), which the classifier does
  not read; those require a coefficient of exactly 1. A bound on `x` may also read its coefficient from the row (`w *
  x >= 1`, `hi * x <= cap2`), as a deterministic nonthrowing source expression; the row allows the values the
  classifier admits over that row's `w`, so a zero `w` allows what `0 >= 1` says. Any other coefficient is a finite
  constant. Contradictory active bounds are infeasible. A NULL or NaN value raises the solver's error on every row the
  bound's `when` keeps (`DirectCheckRowBounds`): it names the NULL column, or quotes a computed value as written; a
  NULL, NaN or infinite coefficient raises the solver's coefficient error. A row the `when` drops is not read.
- **Objective:** a signed sum of per-row coefficients times `x`, `MAXIMIZE` or `MINIMIZE`, plus an optional finite constant,
  or none. Without one every row scores 0, so the plan picks only the rows the lower limits and fixed values require.
  Coefficients are numeric, decision-free, deterministic; several terms must be non-throwing. Each `SUM(...)` may carry a
  constant factor or divisor (`2 * SUM(score*x)`, `SUM(score*x) / 2`, `-1.5 * SUM(...)`, `(1 + 1) * SUM(...)`) that scales
  that part's coefficients: a negative one reverses the part, zero makes every row score 0, and each part keeps its own.
  The factor must be a finite foldable constant, and a divisor must not be zero.

It raises an error wherever the solver does: a `when` that keeps no row (D4), a sum whose own `when` keeps no row (D8),
invalid or NULL bounds on a row that reads them, NULL or non-finite scores, infeasible counts. Every input row is read, so a late bad value raises under `LIMIT 1`
or `COUNT(*)`. When a query has several errors, direct may report a different one than the solver. When scores tie it
may pick different rows than the solver, with the same objective. Without an objective every feasible assignment is
optimal, so which rows it picks is not specified; Gurobi and HiGHS pick different ones as well. The feasibility check is
a flat list of independent checks (a fixed-value conflict, a valid interval, enough rows, a fixed count within the upper
limit, an unrepresentable limit), each true or the infeasible error; which failing one raises is not specified, and all
of them say the same thing.

**Known difference: values within the solver's tolerance of an integer.** Direct solve reads every
bound exactly: counts, constants and column values. The solver accepts a violation of about 1e-6, and the two backends
differ from each other, so there is no single behavior to copy. `SUM(x) <= 1.9999999` selects 2 rows on the solver and 1 on
direct; `when id = 2: x >= 0.000001` fixes the row on Gurobi but not on HiGHS, and `x <= 0.999999` fails inside HiGHS. A
column value computed in floating point (`0.1 * 10`) can land just under 1 and be read as below 1 by direct and as 1 by the
solver. Chosen over declining near-integer bounds because a column value is only known at run time, where the plan cannot
decline it. To avoid the difference, round the bound. `test_s1_direct_reads_bounds_exactly_near_an_integer` pins the
behavior, so changing it is deliberate.

**Error wording** (`../../architecture/policy.md`). The messages are the solver's, built by the same functions. A NULL
count bound, fixed value or `SUM` source column that is one column raises `DECIDE: column "cap" is NULL. Impute it with
COALESCE(cap, 0) or filter those rows out with a WHERE clause.`, and a NaN one `DECIDE constraint right-hand side
contains invalid value (NaN)...`. Neither path names a row: a row's place in the input depends on the scan. A NULL score names the NULL columns of the
score as the solver does (`column "score"`, or `columns "a" and "b"`), and quotes the score only when no column is NULL.
The D4 and D8 messages (`DecideWhenMatchesNoRowsMessage`, `DecideReducerHasNoRowsMessage` in
`decide_value_messages.hpp`), the non-finite score and the infeasible messages are the solver's too. A computed bound or fixed value
(`TRY_CAST(cap AS INTEGER)`) is quoted as written, and an `=` bound that takes two values in a group raises `` `cap`
takes more than one value here (5 and 7) ``, on both paths. When several rows are bad, which one a message names is not
specified.

## What it does not admit (falls back to the solver)

A query S1 does not apply to keeps its answer: it runs on the solver, and `require` raises one fixed error.

**Explicitly out of scope** (these stay on the solver on purpose):

| Shape | Why it is out | How to reopen it |
|---|---|---|
| A right-hand reducer whose own `when` is volatile (`min(when random() < 2: v)`), a relation-qualified one, or another reducer (`MEDIAN`) | Kept on the solver by decision | A separate task per shape |
| A count body with a constant or a column term (`SUM(x + 1)`, `SUM(x + bonus)`) | The class is a count of `x`; the solver answers these exactly | A rewrite to `SUM(x) <op> B - c*COUNT(*)` before both paths, which changes the solver's overflow, `DIAGNOSE` and error text |
| A count body with a weight on `x` other than 1 or -1 (`SUM(2*x + 1)`) | A weighted constraint (S3, S4) | A weighted rule |
| A factor on `SUM(...)` in the objective that is not a usable constant: a zero divisor, a NULL factor, or a subquery factor (`(SELECT 2) * SUM(...)`) | A zero divisor raises on the solver, which a plan built from a constant cannot reproduce. The solver reads a NULL factor as an objective of zeros, which is not a result worth copying. A subquery factor has no sign when the plan is built, and the sign decides whether the sense reverses | Admit a subquery factor once its value can be read before the plan is chosen. Low value |
| A bound on `x` the solver folds (`variable_bound`) that cannot hold: an upper bound below 0 or a lower bound above 1 (`x >= 2`, `x <= -1`, `x < 0`, `x > 1.5`, `x = -1`; a strict bound counts after its shift to a whole number) | The solver raises `x >= 2 cannot hold for a BOOL variable.` before it reads a row, so an empty source raises it too, and under `DIAGNOSE` too. A query that can only fail has no speed to win, so S1 declines it (`DirectContradictsDomain`) and the solver answers. The same bound with a `when` is admitted and raises DECIDE's generic infeasible error from the plan, as the solver does. An affine spelling (`1*x >= 2`) folds too, and S1 declines it the same way | Not planned |

**Belongs to another problem class or is intentional:** weighted constraints such as `SUM(w * x) <= B` (S3 and S4 cover two special cases of them); `INT` and
`REAL` variables (A1 and the R classes); more than one decision, entity or scalar decisions (D1); lower and upper bounds with
different `when` membership (S2); `SUM(x) <> k` (not an interval); bounds that are infinite, NaN or above 2^53
(not exactly representable); an objective with a `when`, a `per` or a two-level reducer, other reducers (`MIN`, `MAX`,
`AVG`), `norm`; `DIAGNOSE`.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_s1_uniform_matroid.py` (578 tests) | The answers. Each case runs on the independent ILP oracle (`oracle_solver`, built from the raw rows), on the solver path (`off`) and on the direct path (`require`, so the test fails if S1 does not apply); both DeciDB runs must satisfy every constraint and reach the oracle's objective, or all must say infeasible. Cases: every comparison against whole and fractional limits (strict bounds as the solver path reads them), intervals, `by` and NULL keys, a `when` in front, inside the sum and in both places, source-valued bounds and `COALESCE`, several bounds at once, row-count bounds (`COUNT(*)`, exact decimal arithmetic, and `test_s1_count_bound_raises_only_where_the_solver_does` for where an overflow may raise), group-aggregate bounds (`MIN`, `MAX`, `COUNT(col)`, small-integer `SUM` and `AVG`, in arithmetic, with `by`, a `when` in front and inside the sum, their NULL, NaN, infinity and empty-group errors, and every declined reducer), negated count bodies (each comparison and an interval, every scope, a group whose own `when` keeps no row, an unreachable limit, constant, column, `COALESCE`, `COUNT(*)` and group-aggregate limits), constant terms with and without `by` and an own `when` included, pins and per-row bounds on `x` (a generated grid with and without `when`), column pins, no objective (every count bound shape, with and without `by` and `when`, checked for feasibility only), 11 objective shapes (sums, differences, negation, scaled and divided, offset, products), the nine beyond the plain score in both senses, and tied scores. Parent-query contexts (CTE, wide source, joined and correlated source, parent join, recombined filters), a nested `DECIDE`, and empty inputs. Error cases that must fail on both paths (a `when` in front that keeps no row, a sum's own `when` that keeps none of the clause's rows, all keys NULL, an infeasible `byR` bound, an equality bound that varies within a group, a NaN fixed value, NaN and empty groups in a group-aggregate bound). Boundary cases against the solver path only (infinite and beyond-2^53 limits, every numeric source type, exact decimal arithmetic) and tiny scores against the exact finite-DOUBLE optimum The direct path of a case with a `by` key is a strict xfail (`direct_path`), a separate dataset covers a `by` group with an own `when` under D8, and `test_direct_source_values.py` marks the grouped slots `xfail(strict=True)`, until S1 reads a `by` key (`todo.md`). |
| `test_direct_rule_conformance.py` (118 tests, shared with A1) | The checks every rule owes: schema and rows, all-rows read, serializer, prepared plans, `EXPLAIN` and profiling, `off`, forced backend, `DIAGNOSE`, an empty source, constant bounds that contradict the built-in domain, and near misses, each of which must decline under `require`, keep the solver plan, and answer under `auto` as under `off` (this is where the shapes S1 leaves on the solver are tested) |
| `test_direct_user_facing.py` (75 tests, shared with A1) | What a SQL user sees besides the answer: direct solve on by default, the pinned exact reading of a bound near an integer, the full text of every NULL and NaN error compared with the solver's, unused wide columns pruned, forced-backend and binder-error policy |
| C++ `[decidb]` | S1 applicability condition, IR, rule driver |

## Performance

Measured 2026-10-13 (macOS arm64, 11 cores): the global narrow rows on top of `95ee8bf58c` with only variable-width columns late-materialized, the rest at `7a480a1a71`, where those shapes plan the same way. 5,000,000 stored rows; every query ran in a fresh process
that only opens the database file, so peak memory is the query's own. Times are median query-plus-collection seconds
(direct 1 to 3 runs, Gurobi 1 or 2). In all 36 runs direct and Gurobi agreed on selected count and objective. HiGHS was
not measured. The grouped rows measure the per-group plan, which no query reaches while S1 declines a `by` key (`todo.md`).

| Shape | Direct | Gurobi | Speedup | Direct peak | Gurobi peak |
|---|---:|---:|---:|---:|---:|
| Global top-K, narrow, full output | 0.18 s | 2.89 s | 16x | 1.0 GiB | 2.3 GiB |
| Global top-K, narrow, every column reduced in the query | 0.17 s | 2.84 s | 17x | 0.9 GiB | 2.2 GiB |
| Global top-K, unused 512 B payload (pruned) | 0.16 s | 3.00 s | 19x | 0.8 GiB | 5.3 GiB |
| 100 groups, fixed values | 0.33 s | 32.4 s | 98x | 0.5 GiB | 6.6 GiB |
| 100 groups, aggregate-local `WHEN` | 0.30 s | 28.4 s | 95x | 0.5 GiB | 6.0 GiB |
| 100 groups, local `WHEN`, source-valued bound | 0.55 s | 27.4 s | 50x | 0.7 GiB | 3.7 GiB |
| 100 groups, source-valued bounds | 0.50 s | 29.6 s | 59x | 0.7 GiB | 6.3 GiB |
| 100 groups, paired source-valued bounds | 0.66 s | 29.6 s | 45x | 0.8 GiB | 6.3 GiB |
| 100 groups, `COALESCE` source-valued bound | 0.57 s | 29.5 s | 52x | 0.6 GiB | 6.3 GiB |

The two global narrow rows return the `BIGINT` key `i`, which stays in the single read (only a variable-width column is
late-materialized; `../../architecture/result_adapter.md` § Late materialization of a stored table). Gurobi's peak
varies by about 2 GiB between identical runs (4.6 to 6.5 GiB on the 100-group shape over four runs): the model it
receives is the same in every run, and the peak is reached inside its presolve, which takes 32 of the 33 s. DeciDB's own
part of those runs peaks at 1.2 GiB.

The speedup comes mostly from the solver being slow here: it builds a model with millions of variables, which direct solve
skips. A shape whose solver model is cheap would show a smaller win.

**Without an objective** (`SUM(x) BETWEEN rows/20 AND rows/10`, one aggregate row, direct 3 runs, solver 1): 0.05 against
0.07 s at 100,000 rows, 0.06 against 0.28 s (85 against 325 MiB) at 1,000,000, and 0.16 against 1.23 s (541 to 657 MiB
against 1.6 GiB) at 5,000,000.

**The NULL check.** Every score is checked for NULL on every run, and the check cannot be switched off, so its cost on
clean data is part of every number above and is not measured apart. With one NULL score on the last row, direct raises
in 0.04 s at 1,000,000 rows and 0.07 s at 5,000,000 (clean: 0.06 and 0.19 s), and the solver in 0.06 and 0.12 s.

**Weak spots**

| Case | Result |
|---|---|
| Wide rows (512 B) returned in full | Direct 0.46 s versus Gurobi 3.40 s, 7.4x; reduced inside the query, 0.46 against 2.95 s. The payload is late-materialized from the stored table, so it never enters the rank. Memory 3.9 GiB versus 6.2 GiB with the full result collected, 1.4 GiB versus 5.4 GiB when reduced. Footprint numbers are in `../../architecture/result_adapter.md` |
| Small inputs, up to 10,000 rows | Everything finishes in about 20 ms, so no real difference (earlier measurement, not re-run) |
| 100,000 wide rows, trivial capacity | Direct about 5% slower than Gurobi, two runs (earlier measurement, not re-run). The only measured loss |
| Capacity 0 or all rows, 5M narrow | Still wins, by 3 to 4x instead of 16x (earlier measurement, not re-run) |

**Reproduce** (after `make release`; the sweep above took about 4.5 minutes):

```sh
clang++ -std=c++17 -O2 -I src/include benchmark/decide/direct_solve/archive/profile_direct_s1_api.cpp \
  -L build/release/src -lduckdb -Wl,-rpath,$PWD/build/release/src \
  -o benchmark/decide/results/profile_direct_s1_api
P="python3 benchmark/decide/direct_solve/archive/profile_direct_s1_api.py --rows 5000000 --db-dir /tmp/s1db --keep-db --timeout 180"
$P --widths 0 512 --output-kinds full consume --modes direct gurobi --repeats 2
$P --widths 512 --output-kinds aggregate --modes direct gurobi --scopes global --cardinalities upper --repeats 2
$P --widths 0 --output-kinds aggregate --modes direct gurobi --scopes grouped_local_when --cardinalities interval --bound-kinds constant source --repeats 1
$P --widths 0 --output-kinds aggregate --modes direct gurobi --scopes grouped --pins fixed --cardinalities interval --repeats 1
$P --widths 0 --output-kinds aggregate --modes direct gurobi --scopes grouped --cardinalities interval --bound-kinds source source_pair source_coalesce --repeats 1
```

Add `--output file.csv` to keep the raw rows. The runner pins `require` for direct and `off` for the solver, so a run never
silently uses the other path. The wide database needs about 3 GiB of disk.
