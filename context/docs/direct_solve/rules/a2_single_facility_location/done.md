# A2 (single-facility location) — what works today

Verified on 2026-11-08 at `73b52ab1d1`: `test/decide/run_direct_matrix.sh`, each of its four suites (auto, off,
serializer, highs) 7,020 passed; C++ `build/release/test/unittest "[decidb]"` 4,635 assertions in 85 cases. Code:
`src/optimizer/decide/direct/a2_single_facility_location_rule.cpp`. Class definition: [definition.md](definition.md).

## What it admits

One `REAL` decision shared by many rows, query-wide (`per ():`) or keyed (`per k:`, `per k, j:`, `per D:`), with:

- **The key.** One problem per class of the key. A row whose column element of the key is NULL is in no class: it has
  no decision, its `c` is NULL, and nothing on it enters the objective, as on the solver path. An element that stands
  for a relation (`per D:`) takes NULL as a value of its own. The IR carries the key as `DirectKeyElement`s
  (`DirectIRKeyedScope::key`, read by the same `ReadKey` as a `by` key), and the plan partitions by it through
  `DirectProjectScope`.
- **The interval.** Per class, the decision's box (the default floor of 0 or a written constant lower bound, and a
  constant upper bound) tightened by every bound clause: `MAX` of each clause's lower ends and `MIN` of its upper ends
  over the rows of the class it keeps (`DirectTightestBound`). Constant and data bounds are one reading; a constant
  folds. As on the solver path, a NULL bound is skipped, an infinite bound in the open direction is no bound, an
  upper bound of `-inf` is infeasible, and a data lower bound does not lift the default floor of 0. A constant lower
  bound under a `when` lifts the floor for the whole class when some row of the class keeps it (`DirectStaticFloor`
  over a per-class `bool_or`).
- **The objective.** One part holding one loss of `w*c + t`, where the slope `w` folds to a nonzero constant
  (`DirectFoldConstantSum`) and `t` is a value of the source row; an objective `when` and the reducer's own `when`
  choose the rows the loss reads. `POWER(c - v, 2)`, `POWER(v - c, 2)`, `POWER(2 * c - v, 2)`, `3 * SUM(...) / 2`,
  `MAXIMIZE -SUM(...)` and an objective constant (`+ 5`) all read through one formula, with no case list.
  - **Squared:** `SUM(k * POWER(w*c + t, 2))`, where the weight `k` is a constant or a value of the source row, of
    any sign.
  - **Absolute:** `SUM(k * ABS(w*c + t))` and `norm(c - v, 1)`, where `k` is a constant, positive once the sense is
    applied.
  - **Maximum absolute:** `MINIMIZE MAX(k * ABS(w*c + t))` and `MINIMIZE norm(c - v, 'inf')`, where `k` is a positive
    constant.

**The answer.** Each loss gives a target per class; the plan clamps it into the class's interval and repeats it on
every row of the class. When several points are optimal, A2 returns the one nearest 0 (the owner's ruling). The clamp
of 0 gives that point with no branch of its own, and it stays finite when an end is open.

| Loss | Target |
|---|---|
| Squared, total weight `a = SUM(k*w^2)` above 0 | The turning point `-b / (2a)` with `b = SUM(2*k*w*t)`: the weighted mean of the values for `POWER(c - v, 2)` |
| Squared, `a` of 0 or less | An end of the interval: the upper one when `a*(L+U) + b < 0` (as `f(U) - f(L) = (U-L)*(a*(U+L) + b)`, so no large end is squared; `b` alone when `a = 0`), the lower one when it is above 0; on a tie the end nearest 0, the lower one between `-U` and `U`, and 0 when `a = b = 0` |
| Absolute | The point nearest 0 between the lower and the upper weighted median of the centers `v = -t/w`, each weighted by `k*|w|` |
| Maximum absolute | The midpoint of the least and the greatest center, as `lo/2 + hi/2` so the sum of the ends cannot pass a DOUBLE |
| No kept row in the class | None: every point is optimal, and the answer is the point nearest 0 |

A keyed maximum is one maximum over every row of every class; each class's own midpoint is optimal for it, and the
tie rule applies within each class's own problem.

**Errors and warnings, as on the solver path.** A NULL value or weight reads as no term on its row. A NaN or infinite
value or weight on a row the objective keeps raises the solver's error, and so does a NaN bound on a row its clause
keeps, whether or not the row has a class. An empty interval raises the infeasible error on every row that has a
class; over constant bounds the check is two constants, which DuckDB folds. A concave or linear squared loss whose
chosen end is infinite raises the unbounded error. A sum past a DOUBLE (the square's `a` or `b`, or an absolute loss's
total weight) raises "computes a number too large for a DOUBLE" with the objective as written, and so does a convex
turning point past a DOUBLE at an open end. An empty source returns no rows, whatever the interval says. Nothing is
computed on a row a `when` drops. The precision warning fires where a divisor is below the normal range: the square's
`a` in `-b / (2a)` (`DirectTurningPoint`), or a center's slope or a nonzero constant beside it.

## How it works

Group-then-join: the class plan computes each class's answer once, on one row per class, and the rows join it back
on their key. `Rule::Apply` builds the class plan over a first read of the source (`DirectBuildOverRead`) and the
join over a second (`DirectBuildOverOneRead`); a source that is not a plain scan is read once into a materialized CTE
and scanned twice, so a `random()` or a costly join is not computed twice. `PlanBuilder` is the skeleton of a Template Method with one method per stage, and
each loss is a Strategy (`Loss`: `SquareLoss`, and `AbsoluteLoss` and `MaxAbsoluteLoss` over `CenteredLoss`) that
fills in its columns at each stage and its target. A loss keeps its columns by name in a struct of its own
(`LossColumns`: `SquareColumns`, `MedianColumns`, `ExtremeColumns`), which the skeleton makes once
(`Loss::NewColumns`) and hands to every stage without reading it, as DuckDB hands a `GlobalSinkState` to a sink; the
loss casts it back (`Cast<T>`). The shared tail (the clamp of 0 for no target, the clamp into the interval, NULL on a
row with no class, the infeasible error, the too-large check of the answer) is in one place. It runs the loss's
checks, then the answer's too-large check where the loss asks for one (`Answer::finite_where`: the squared loss, on a
convex class), then the loss's warnings, since a warning prints as it runs. The objective as written, which the
too-large message names, is held once, by the rule's condition.

1. Scope (`DirectProjectScope`, keyed decisions only): the row's class key (`DirectClassKey`, the framework's [class
   key](../../framework/done.md)). A row in no class has a NULL in each column of the key compared with `=`, and no flag
   column stands beside the key.
2. P1 (`LayoutRawValues`): the loss's raw values on the rows the objective keeps (`DirectLayoutQuadraticPart`: weight,
   slope and constant) and each bound clause's values on the rows it keeps (`DirectLayoutBoundClauses`, shared with
   A1), with the check of every value that can fail.
3. P2 (`ComputeRows`): the loss's row values (a square's `a` and `b`; a center and its weight), each clause's ends on
   the row (`DirectClauseBounds`), and the row's checks.
4. C (`AggregateClasses`): one hash aggregate grouped by the key (ungrouped for a query-wide decision) holding the
   loss's aggregates (the sums of `a` and `b`; the least and greatest center), each clause's tightest ends through
   `DirectAggregateSink`, the `bool_or` of each floor-lifting `when`, and the `bool_and` of the rows' validity. The
   absolute loss adds the lower median `quantile_disc(c, 0.5)` and the upper median `-quantile_disc(-c, 0.5)` of the
   centers, and their total weight for the too-large check. A plain median is the weighted one here because every
   center has the same weight `m = k*|w|`: A2 admits only a constant positive `k` and a constant nonzero slope `w`
   (`ProveAbsolute`), and a NULL guard that skips a row's center takes its weight with it. The aggregate holds every
   row before it gives one, so each row's check has run on all of them first, as the solver reads every row before it
   answers. No step of the plan is a window, so every step runs on every thread.
5. P3 (`ComputeInterval`): the class's interval as columns, with the floor; the loss forwards its class values
   through it (`CarryClasses`), since P4 reads only P3's columns.
6. P4 (`ComputeAnswer`), on the class's row: the target clamped into the interval (`DirectRowBox`; `+ 0.0` turns
   `-0.0` into `0.0`; this `+ 0.0` is not an IEEE 754 identity, so the rewriter keeps it), NULL on a key with no class
   (`DirectClassPresent`), and the column that keeps the checks live. The class's validity
   is the first check (`DirectAllHold`), so an empty source's one query-wide row, whose `bool_and` is NULL, runs no
   check.
7. Join (`JoinAnswers`, `DirectJoinClassAnswers`): the second read takes its scope again and joins each row to its
   class's answer, a SINGLE join on the class key, each column compared by what a NULL means in it, so a row with no
   class matches no class row; or a cross product with the one row of a query-wide decision. DuckDB runs a join on one
   integer `=` column as a perfect hash join. P4 sits below the join, so no joined row exists before its class's
   row has passed its checks; the SINGLE join keeps the classes on the build side and the rows stream through the
   probe.
8. P5: the source columns, the answer and the check column.

## What it does not admit (falls back to the solver)

A query A2 does not apply to keeps its answer: it runs on the solver, and `require` raises one fixed error. Open work
on these shapes is in [todo.md](todo.md).

| Shape | Why |
|---|---|
| `SUM(w * ABS(c - v))` with `w` read from the data | The median is optimal only for nonnegative weights, and the plan is chosen before any weight is read. |
| `norm(c - v, 0)` | The L0 loss: its proof is long for a rare loss, and the IR marks it not proved. |
| `MAXIMIZE` of an `ABS` maximum, `MAX(w * ABS(...))` with a weight read from the data or not positive | Not the midpoint. |
| A linear term or a per-row constant beside the loss, a second loss term, a second part | Not one loss. |
| `MINIMIZE -SUM(ABS(...))` and other concave absolute losses | Not convex, and not the median. |
| `AVG(...)` objective, no objective, a slope of 0 or read from the data, a slope past a DOUBLE | Not one plain loss of `w*c + t`. |
| A bound below the built-in floor of 0 (`c <= -1` with no lower bound) | The bound reader declines it so the solver reports it, as in A1. |
| `INT`, `BOOL`, a decision per row, two decisions | Not one shared `REAL`. |

## Where it differs from the solver

- **Its own warning.** The divisors are the square's `a` and each center's slope; the rule is the framework's
  [Number-range warning](../../framework/done.md).
- **Ties.** Where several points are optimal, the solver may return any of them; A2 returns the one nearest 0.
- **HiGHS and a weight read from the data.** The solver path refuses a data weight on a square under HiGHS (its sign is
  known only when the query runs); A2 answers it, as Gurobi does.
- **A square's constant past a DOUBLE** (`v = 1e200`): the solver path expands the square and refuses once its
  constant `k*t^2` passes a DOUBLE; A2 answers the true center, which never reads that constant
  (`../../architecture/rules.md` § What every rule must match from the solver).

## Tests

- Conformance: the `A2` fixture in `test/decide/tests/_direct_rule_fixtures.py` (the worked example; near misses that
  decline, L0 among them, and forms that apply, keyed, with data bounds, a data weight and an `ABS` among them; late
  NaN values and bounds, query-wide and keyed, and a too-large error, on the last of 5,000 rows).
- Differential oracle: `test/decide/tests/test_direct_a2_single_facility_location.py`: query-wide, keyed, data-bound,
  weighted, absolute and maximum-absolute case tables against the oracle, one case per branch of the plan, each named
  for it (the lower and the upper median, a clamp up, down or none, the nearest-0 tie, no kept row, a NULL value, a
  point interval; each spelling once). The oracle accepts any optimal point; the same test checks, on its `require`
  run, the ruled tie point derived by enumeration (`_ruled_point`) and that `-0.0` prints as `0.0`. Parent contexts;
  random problems; error and boundary tables, a turning point past a DOUBLE
  among them (`test_a2_a_turning_point_past_a_double_*`: a finite end answers it, an open end is too large); the
  infeasible and unbounded texts on both backends; the warnings; HiGHS's refusal of a data weight.
- Error text: `test_a2_*_word_for_word` in `test_direct_user_facing.py` (values, keyed values, NaN data bounds, NaN
  weights, the unbounded error).
- C++: `A2AppliesShapes` and `A2DeclineShapes` in `test/common/test_decidb_direct_decline.cpp`, one decline per
  `CheckCondition` branch, and the partition test against every other rule; the keyed scope's key and the guarded
  square weight in `test_decidb_direct_ir.cpp`.

## Performance

**Against DuckDB's own plan.** Measured at `sf=1` (800,000 rows of `partsupp`), best of 5 in one process, with the
machine at a load of about 4 on 11 cores. The reference is a query written by hand in SQL that computes the same answer
with the same DOUBLE arithmetic.

| Query | Direct | Hand-written | Ratio | Summed operator CPU, ratio |
|---|---:|---:|---:|---:|
| `a2` | 5.5 ms | 3.5 ms | 1.58 | 1.18 |
| `a2_keyed` | 9.9 ms | 7.2 ms | 1.38 | 1.17 |
| `a2_abs` | 17.6 ms | 14.9 ms | 1.18 | 1.16 |
| `a2_abs_global` | 16.1 ms | 16.1 ms | 1.00 | not measured |

On all threads every direct query of this sweep takes 0.22 to 0.50 of its one-thread time. At `sf=0.125` the queries
of 2 to 8 ms take 1.01 to 1.04 times as long on all threads as on one, as DuckDB's own plans do (1.01 to 1.07). The
latency gap of `a2` (ratio 1.58) is the plan's shape: the plan reads its source more than once. It is open work.

Measured with `benchmark/decide/direct_solve/run.py --sf 0.125 0.25 0.5 1 --queries a2 a2_keyed a2_abs` (Gurobi) and
the same with `--modes direct highs` on 2026-11-07 at `09f510c7e9` (median of 3 runs, load about 4 to 6). Every size
agreed with both solvers on the objective within 1e-6, and no solver run failed. All three queries choose a price over
`partsupp`'s `ps_supplycost` in `[0, 2000]`: `a2` one query-wide center under the squared loss, `a2_keyed` one per
supplier (10,000 classes at `sf=1`), `a2_abs` one median per supplier under the absolute loss. Each speedup divides
the solver's median by direct's median from the same sweep.

| Query | Rows | Direct | Direct, 1 thread | Direct peak | Gurobi | Gurobi peak | HiGHS | HiGHS peak | Speedup (Gurobi, HiGHS) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `a2` | 100,000 | 0.034 s | 0.033 s | 38 MiB | 0.032 s | 38 MiB | 0.028 s | 28 MiB | 0.9x, 0.8x |
| `a2` | 200,000 | 0.042 s | 0.042 s | 42 MiB | 0.037 s | 43 MiB | 0.033 s | 34 MiB | 0.9x, 0.8x |
| `a2` | 400,000 | 0.061 s | 0.060 s | 49 MiB | 0.050 s | 54 MiB | 0.046 s | 45 MiB | 0.8x, 0.7x |
| `a2` | 800,000 | 0.094 s | 0.094 s | 64 MiB | 0.075 s | 76 MiB | 0.071 s | 66 MiB | 0.8x, 0.8x |
| `a2_keyed` | 100,000 | 0.034 s | 0.043 s | 79 MiB | 0.036 s | 42 MiB | 0.034 s | 44 MiB | 1.1x, 1.0x |
| `a2_keyed` | 200,000 | 0.037 s | 0.059 s | 92 MiB | 0.048 s | 50 MiB | 0.056 s | 88 MiB | 1.3x, 1.5x |
| `a2_keyed` | 400,000 | 0.044 s | 0.093 s | 124 MiB | 0.072 s | 67 MiB | 0.133 s | 247 MiB | 1.6x, 2.9x |
| `a2_keyed` | 800,000 | 0.059 s | 0.161 s | 184 MiB | 0.127 s | 99 MiB | 0.391 s | 851 MiB | 2.2x, 6.5x |
| `a2_abs` | 100,000 | 0.058 s | 0.063 s | 76 MiB | 0.863 s | 593 MiB | 2.985 s | 381 MiB | 14.9x, 63.1x |
| `a2_abs` | 200,000 | 0.059 s | 0.099 s | 94 MiB | 2.058 s | 1,378 MiB | 8.072 s | 746 MiB | 34.6x, 133.0x |
| `a2_abs` | 400,000 | 0.084 s | 0.173 s | 136 MiB | 4.769 s | 2,639 MiB | 22.009 s | 1,455 MiB | 56.7x, 254.2x |
| `a2_abs` | 800,000 | 0.139 s | 0.324 s | 251 MiB | 11.964 s | 4,416 MiB | 62.359 s | 2,782 MiB | 86.1x, 461.2x |

Times include process start and database open, about 20 ms, which is most of the direct time at the small sizes.

The query-wide squared loss is slower than both solvers, at 0.7 to 0.9 times their speed. The solver path folds a
query-wide decision into one variable whose coefficients are the same two sums, so it does A2's arithmetic too; A2's
plan computes the answer and its checks once per row, and its window, with no key to partition by, hands every row
above it to one thread (`direct` and `direct, 1 thread` time the same). Framework task H-48 holds the fix
(`../../framework/todo.md`). The keyed squared loss is ahead (1.0 to 6.5 times), as the same per-row work runs in
parallel. The absolute loss is 15 to 86 times faster than Gurobi and 63 to 461 times faster than HiGHS, which solve
one helper decision per row, and needs a tenth (HiGHS) to a seventeenth (Gurobi) of their peak memory at `sf=1`. The
keyed squared loss takes more memory than Gurobi (184 MiB against 99 MiB at `sf=1`): its window holds every row,
where the solver keeps one variable per class.
