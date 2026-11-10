# S1 (uniform-matroid optimization) — what works today

Verified on 2026-11-08 at `73b52ab1d1`: `test/decide/run_direct_matrix.sh`, each of its four suites (auto, off,
serializer, highs) 7,020 passed; C++ `build/release/test/unittest "[decidb]"` 4,635 assertions in 85 cases.

Code: `src/optimizer/decide/direct/s1_uniform_matroid_rule.cpp`; its scope projection and bound validation are the
shared fragments in `direct_plan_fragments.{hpp,cpp}`. Class definition: [definition.md](definition.md).

## What it admits

One row-scoped `BOOL` decision `x` (output is `INTEGER` 0/1), with:

- **Count bounds on `SUM(x)`:** `<=`, `<`, `>=`, `>`, `=`, or several combined into one interval. A bound can be a constant,
  a foldable constant expression, or a numeric source column or deterministic expression over source columns that varies per
  row, including one that can raise (`cap + 1`, a narrowing `CAST`). A data bound reaches S1 as a group bound
  (`../../framework/done.md`): S1 computes its aggregates (the MIN of an upper bound, the MAX of a lower one, an `=` bound's
  MIN and MAX that must agree) once per group, in a hash aggregate over a first read of the source joined back to the
  rows before the count filter (group-then-join, as A2), and raises its NaN and disagreement errors under the plan,
  in the solver's words (a NULL skips the instance). S1 reads that value in its score projection, on the rows its clause
  keeps; nothing is computed on a row the `when` drops (syntax reference §6.3). Values are converted to DOUBLE as the
  solver does; strict and fractional limits become inclusive integer counts; limits are exact up to 2^53.
- **Bounds read from the kept rows:** `COUNT(*)`, `COUNT`, `MIN`, `MAX`, `SUM` and `AVG` of a numeric source expression on
  the right of a count bound, alone or inside `+ - * / //` and other arithmetic (`SUM(x) <= COUNT(*) / 2`,
  `>= COUNT(*) * 0.1`, `<= MIN(cap)`, `>= MAX(lo) / 2`, `<= SUM(cap)`), with or without a `when` of its own
  (`<= sum(when f: 1)`, `<= min(when cap > 1: cap)`), a written norm and a keyed or colon reducer. A reducer over exactly the clause's
  group is a group bound, which S1 computes in that aggregate; any other reducer reaches S1 as a source column the
  bound-side rewrite built (`../../framework/done.md`). S1 has no reader of its own for a reducer. The value is DuckDB's, in its own types: `SUM` over any numeric type is
  exact, a bound of `COUNT(*) * 0.57` over 100 rows is exactly 57, and over the `BIGINT` values 2^60, 1, -2^60
  `SUM(cap)` is 1. `COUNT(*)` counts the rows the clause's `when` keeps; a `when` inside the left-hand sum
  (`SUM(when active: x) <= COUNT(*) / 2`) does not narrow the right. An infinite value is a value. An overflow
  (`COUNT(*) * 4611686018427387904` over 3 rows) raises as on the solver path, and only on a row the clause keeps. The use
  case is a top-fraction query ("the best 10% of the rows": `SUM(x) <= COUNT(*) * 0.1`). A reducer with a `by` key gives
  each group its own count interval (one GROUP BY on the key, joined back to the rows); a NULL key is in no group (D5),
  and a `per K:` clause bounds the whole count once per class (D6).
- **Count bodies:** `SUM(a*x)` where the terms in `x` add up to exactly `a = 1` or `a = -1` (`SUM(x)`, `SUM(1*x)`,
  `SUM(2*x - x)`, `SUM(-x)`); `SUM(-x) <op> B` is `SUM(x) <flipped op> -B`. A body with a constant term (`SUM(x + 1)`,
  `SUM(1 - x)`, `SUM(x + 0.5)`) declines, and the solver answers it. The class asks only for a count of `x`, and
  admitting a constant meant shifting the limit by `n*c` per group with an exactness cap and its own overflow cases.
- **Scope:** global, optionally with a deterministic source-only `when` in front of the clause, inside the sum, or
  both (the sum then counts the rows both keep). All count clauses must share identical membership. Rows the sum's own `when` leaves out are outside the count but still obey
  per-row fixed values. A `when` that keeps no row, or a sum whose own `when` keeps no row, leaves the clause with no
  instance, as on the solver (D68).
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
  constant. Contradictory active bounds are infeasible. A NULL value or coefficient skips the bound on that row, and a
  NaN value raises the solver's error on every row the bound's `when` keeps (`DirectRowBoundMessage`); a NaN or
  infinite coefficient raises the solver's coefficient error. A row the `when` drops is not read.
- **Objective:** a signed sum of per-row coefficients times `x`, `MAXIMIZE` or `MINIMIZE`, plus an optional finite constant,
  or none. Without one every row scores 0, so the plan picks only the rows the lower limits and fixed values require.
  Coefficients are numeric, decision-free, deterministic; several terms must be non-throwing. Each `SUM(...)` may carry a
  constant factor or divisor (`2 * SUM(score*x)`, `SUM(score*x) / 2`, `-1.5 * SUM(...)`, `(1 + 1) * SUM(...)`) that scales
  that part's coefficients: a negative one reverses the part, zero makes every row score 0, and each part keeps its own.
  The factor must be a finite foldable constant, and a divisor must not be zero. The objective may have a `when` in
  front, and each `SUM` one inside (`MAXIMIZE when f: SUM(s*x)`, `MAXIMIZE SUM(when f: s*x)`), each a deterministic
  source predicate that cannot raise. A row they drop (false or NULL) scores 0 in that part and reads no coefficient
  there, so a NaN or a value that raises on it does not raise; the row still counts toward the bounds. A `when` that
  keeps no row leaves every score 0.

It raises an error wherever the solver does: NaN bounds on a row that reads them, NaN or infinite scores, infeasible counts. Every input row is read, so a late bad value raises under `LIMIT 1`
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

**Error wording** (`../../architecture/policy.md`). The messages are the solver's, built by the same functions. A NaN
count bound, fixed value or `SUM` source column raises `DECIDE: the bound cap is NaN on some rows. Skip those rows
with 'when NOT isnan(cap):'.`. Neither path names a row: a row's place in the input depends on the scan. A NULL is
skipped on both paths: a NULL limit is no limit, and a NULL score leaves its row out of the objective.
The non-finite score and the infeasible messages are the solver's too. A computed bound or fixed value
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
| A factor on `SUM(...)` in the objective that is not a usable constant: a zero divisor or a subquery factor (`(SELECT 2) * SUM(...)`) | A zero divisor raises on the solver, which a plan built from a constant cannot reproduce. A subquery factor has no sign when the plan is built, and the sign decides whether the part reverses | Admit a subquery factor once its value can be read before the plan is chosen. Low value |
| A bound on `x` the solver folds (`variable_bound`) that cannot hold: an upper bound below 0 or a lower bound above 1 (`x >= 2`, `x <= -1`, `x < 0`, `x > 1.5`, `x = -1`; a strict bound counts after its shift to a whole number) | The solver raises `x >= 2 cannot hold for a BOOL variable.` before it reads a row, so an empty source raises it too, and under `DIAGNOSE` too. A query that can only fail has no speed to win, so S1 declines it (`DirectContradictsDomain`) and the solver answers. The same bound with a `when` is admitted and raises DECIDE's generic infeasible error from the plan, as the solver does. An affine spelling (`1*x >= 2`) folds too, and S1 declines it the same way | Not planned |

**Belongs to another problem class or is intentional:** weighted constraints such as `SUM(w * x) <= B` (S3 and S4 cover two special cases of them); `INT` and
`REAL` variables (A1 and the R classes); more than one decision, keyed or query-wide decisions (D1); lower and upper bounds with
different `when` membership (S2); `SUM(x) <> k` (not an interval); bounds that are infinite, NaN or above 2^53
(not exactly representable); an objective with a `per` or a two-level reducer, other reducers (`MIN`, `MAX`, `AVG`),
`norm`; an objective `when` that is volatile or can raise; `DIAGNOSE`.

## How it works

The plan is a scope projection, a score projection, a rank window with the feasibility filter, and the assignment
projection. The group bounds' aggregates are computed once per group, by a hash aggregate over a first read of the
source, and joined to the rows of the window (group-then-join, as A2).

- **Class key.** A scoped problem carries the framework's [class key](../../framework/done.md): the `by` key's
  elements, compared with `=` where a NULL puts a row in no group, and one marker column when a `when` can drop rows
  and no element says so. The rank and count windows partition by it, so a row in no group ranks apart from every
  group, and the group bounds join on it, so that row meets no group's bounds. No BOOLEAN eligibility column is part of
  the key.
- **The score's message.** The score projection carries a message column, the least of which the plan raises, only for a
  score that can fail: the sum of the bounds of its terms, with their factors, is not below half the range of a DOUBLE
  (the framework's [Number messages](../../framework/done.md)). A score made of values of exact types has no message
  column, and a per-row bound of an exact type is not checked for NaN.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_s1_uniform_matroid.py` (627 tests) | The answers. Each case runs on the independent ILP oracle (`oracle_solver`, built from the raw rows), on the solver path (`off`) and on the direct path (`require`, so the test fails if S1 does not apply); both DeciDB runs must satisfy every constraint and reach the oracle's objective, or all must say infeasible. Cases: every comparison against whole and fractional limits (strict bounds as the solver path reads them), intervals, `by` and NULL keys, a `when` in front, inside the sum and in both places, source-valued bounds and `COALESCE`, several bounds at once, row-count bounds (`COUNT(*)`, exact decimal arithmetic, and `test_s1_count_bound_raises_only_where_the_solver_does` for where an overflow may raise), group-aggregate bounds (`MIN`, `MAX`, `COUNT(col)`, small-integer `SUM` and `AVG`, in arithmetic, with `by`, a `when` in front and inside the sum, their NULL, NaN and infinity errors, a group they read no row of, and every declined reducer), negated count bodies (each comparison and an interval, every scope, a group whose own `when` keeps no row, an unreachable limit, constant, column, `COALESCE`, `COUNT(*)` and group-aggregate limits), constant terms with and without `by` and an own `when` included, pins and per-row bounds on `x` (a generated grid with and without `when`), column pins, no objective (every count bound shape, with and without `by` and `when`, checked for feasibility only), 11 objective shapes (sums, differences, negation, scaled and divided, offset, products), the nine beyond the plain score in both senses, objective `when` shapes (in front, inside the sum, in both places, on one part and on every part, a NULL `when`, one that keeps no row, beside a count `when` and a `by` key, over a NULL coefficient, on rows whose coefficient is NaN or raises, and the NaN error on a row it keeps, word for word with the solver's), and tied scores. Parent-query contexts (CTE, wide source, joined and correlated source, parent join, recombined filters), a nested `DECIDE`, and empty inputs. Error cases that must fail on both paths (an infeasible `byR` bound, an equality bound that varies within a group, a NaN fixed value, NaN in a group-aggregate bound); empty sets on both paths are in `test_empty_groups.py`. Boundary cases against the solver path only (infinite and beyond-2^53 limits, every numeric source type, exact decimal arithmetic) and tiny scores against the exact finite-DOUBLE optimum. Grouped cases run on both paths, with NULL keys and per-class differential cases. The three oracle grids (482 cases) keep one case per path through the DECIDE pipeline (`make decide-coverage`); where cases share a path, each value on a boundary the code or DuckDB's operators treat differently stays (zero, a whole number against a fraction, the edge of a binary domain, the largest exact double, `MIN` against `MAX`), since the values reach DuckDB's sort, rank, filter and aggregate operators, which coverage does not see. |
| `test_direct_rule_conformance.py` (102 tests, shared with every rule) | The checks every rule owes: schema and rows, all-rows read, serializer, prepared plans, `EXPLAIN` and profiling, `off`, forced backend, `DIAGNOSE`, an empty source, constant bounds that contradict the built-in domain, and near misses, each of which must decline under `require`, keep the solver plan, and answer under `auto` as under `off` (this is where the shapes S1 leaves on the solver are tested) |
| `test_direct_user_facing.py` (108 tests, shared with every rule) | What a SQL user sees besides the answer: direct solve on by default, the pinned exact reading of a bound near an integer, the full text of every NaN error compared with the solver's, a NULL bound skipped on both paths, unused wide columns pruned, forced-backend and binder-error policy |
| C++ `[decidb]` | S1 applicability condition, IR, rule driver |

## Performance

Measured on 2026-11-09 at `0ab6224c60`, on an 11-core Apple M3 Pro at a load of about 3.

**Against a hand-written plan.** `benchmark/decide/direct_solve/run.py --compare --sf 1 --queries s1 s1_global` runs the
direct plan beside a query written by hand for the same answer (`hand/<query>.sql`), with the same checks and the same
DOUBLE arithmetic, in one process, best of 5, over the 800,000 rows of `partsupp`. CPU is every operator's time summed;
latency is on all threads; the planning DECIDE adds is apart. Both sides gave the same answer.

| Query | Direct CPU | Hand CPU | CPU ratio | Direct latency | Hand latency | DECIDE planning |
|---|---:|---:|---:|---:|---:|---:|
| `s1` | 151.2 ms | 147.3 ms | 1.03 | 17.4 ms | 17.1 ms | 0.07 ms |
| `s1_global` | 134.9 ms | 126.0 ms | 1.07 | 20.2 ms | 18.1 ms | 0.19 ms |

Both are within the measurement's noise of the hand plan: the plan is the hand plan's rank window and the rule's check.

**Against the solvers.** `run.py --sf 0.125 0.25 0.5 1 --queries s1 s1_global` (Gurobi) and the same with `--modes
direct highs`: a fresh `decidb` process per run, median of 3, load 3 to 6. At every size each solver's objective agreed
with direct solve's within 1e-6. A speedup divides the solver's median by direct's median from the same sweep. Times
include process start and database open, about 20 ms, which is most of the direct time at these sizes.

| Query | Rows | Direct | Direct, 1 thread | Direct peak | Gurobi | Gurobi peak | HiGHS | HiGHS peak | Speedup (Gurobi, HiGHS) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `s1` | 100,000 | 0.026 s | 0.029 s | 44 MiB | 0.493 s | 100 MiB | 0.089 s | 99 MiB | 19.0x, 3.8x |
| `s1` | 200,000 | 0.027 s | 0.038 s | 56 MiB | 0.717 s | 162 MiB | 0.176 s | 170 MiB | 26.6x, 5.9x |
| `s1` | 400,000 | 0.032 s | 0.058 s | 80 MiB | 1.437 s | 294 MiB | 0.349 s | 317 MiB | 44.8x, 10.1x |
| `s1` | 800,000 | 0.037 s | 0.091 s | 120 MiB | 2.909 s | 563 MiB | 0.672 s | 601 MiB | 78.2x, 17.2x |
| `s1_global` | 100,000 | 0.025 s | 0.025 s | 34 MiB | 0.365 s | 103 MiB | 0.108 s | 98 MiB | 14.6x, 4.5x |
| `s1_global` | 200,000 | 0.029 s | 0.032 s | 46 MiB | 0.276 s | 142 MiB | 0.248 s | 173 MiB | 9.5x, 7.8x |
| `s1_global` | 400,000 | 0.035 s | 0.049 s | 75 MiB | 0.591 s | 249 MiB | 0.555 s | 321 MiB | 16.8x, 15.2x |
| `s1_global` | 800,000 | 0.040 s | 0.069 s | 118 MiB | 0.704 s | 465 MiB | 0.999 s | 610 MiB | 17.7x, 24.7x |

The solver path builds one binary decision per row; HiGHS solves the per-supplier model faster than Gurobi. Direct solve
is ahead of both at every size, 18 to 78 times Gurobi and 17 to 25 times HiGHS at 800,000 rows, in a fourth to a fifth
of their peak memory.