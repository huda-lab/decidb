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
  - **Absolute:** `SUM(k * ABS(w*c + t))` and `norm(c - v, 1)`, where `k` is a constant, positive once the part's sign
    in the minimization is applied.
  - **Maximum absolute:** `MINIMIZE MAX(k * ABS(w*c + t))`, `MAXIMIZE -MAX(k * ABS(w*c + t))` and
    `MINIMIZE norm(c - v, 'inf')`, where `k` is a constant, positive once the part's sign in the minimization is
    applied.

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

Measured on 2026-11-09 at `0ab6224c60`, on an 11-core Apple M3 Pro at a load of about 3.

**Against a hand-written plan.** `benchmark/decide/direct_solve/run.py --compare --sf 1 --queries a2 a2_keyed a2_abs
a2_abs_global` runs the direct plan beside a query written by hand for the same answer (`hand/<query>.sql`), with the
same checks and the same DOUBLE arithmetic, in one process, best of 5, over the 800,000 rows of `partsupp`. CPU is every
operator's time summed; latency is on all threads; the planning DECIDE adds is apart. Both sides gave the same answer.

| Query | Direct CPU | Hand CPU | CPU ratio | Direct latency | Hand latency | DECIDE planning |
|---|---:|---:|---:|---:|---:|---:|
| `a2` | 20.3 ms | 17.4 ms | 1.16 | 6.0 ms | 3.6 ms | 0.84 ms |
| `a2_keyed` | 36.2 ms | 31.6 ms | 1.15 | 9.7 ms | 7.2 ms | 0.86 ms |
| `a2_abs` | 151.7 ms | 119.6 ms | 1.27 | 32.7 ms | 20.7 ms | 0.52 ms |
| `a2_abs_global` | 30.4 ms | 21.1 ms | 1.44 | 15.1 ms | 13.3 ms | 0.34 ms |

The hand plans carry the rule's checks, so the 15 to 44% more CPU is the rule's general plan against one written for the
query. `a2_abs_global` also checks that the median is not below DOUBLE's normal range, which a DECIMAL column cannot
produce; proving that while planning needs a magnitude analysis, not done for its 0.6 ms (`../../framework/todo.md` §
How to work any task, Small gains).

**Against the solvers.** `run.py --sf 0.125 0.25 0.5 1 --queries a2 a2_keyed a2_abs a2_abs_global` (Gurobi) and the same
with `--modes direct highs`: a fresh `decidb` process per run, median of 3, load 3 to 6. At every size each solver's
objective agreed with direct solve's within 1e-6. A speedup divides the solver's median by direct's median from the same
sweep. Times include process start and database open, about 20 ms, which is most of the direct time at these sizes.

| Query | Rows | Direct | Direct, 1 thread | Direct peak | Gurobi | Gurobi peak | HiGHS | HiGHS peak | Speedup (Gurobi, HiGHS) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `a2` | 100,000 | 0.023 s | 0.023 s | 24 MiB | 0.033 s | 38 MiB | 0.027 s | 29 MiB | 1.4x, 1.1x |
| `a2` | 200,000 | 0.024 s | 0.026 s | 25 MiB | 0.040 s | 44 MiB | 0.031 s | 34 MiB | 1.7x, 1.4x |
| `a2` | 400,000 | 0.024 s | 0.029 s | 26 MiB | 0.050 s | 54 MiB | 0.048 s | 45 MiB | 2.1x, 1.9x |
| `a2` | 800,000 | 0.026 s | 0.038 s | 29 MiB | 0.077 s | 76 MiB | 0.073 s | 66 MiB | 3.0x, 2.7x |
| `a2_keyed` | 100,000 | 0.025 s | 0.024 s | 26 MiB | 0.039 s | 42 MiB | 0.035 s | 44 MiB | 1.6x, 1.4x |
| `a2_keyed` | 200,000 | 0.025 s | 0.027 s | 27 MiB | 0.049 s | 50 MiB | 0.055 s | 88 MiB | 2.0x, 2.4x |
| `a2_keyed` | 400,000 | 0.028 s | 0.036 s | 53 MiB | 0.074 s | 67 MiB | 0.151 s | 247 MiB | 2.6x, 5.1x |
| `a2_keyed` | 800,000 | 0.029 s | 0.046 s | 63 MiB | 0.121 s | 99 MiB | 0.479 s | 851 MiB | 4.1x, 14.4x |
| `a2_abs` | 100,000 | 0.027 s | 0.026 s | 31 MiB | 1.028 s | 602 MiB | 3.141 s | 378 MiB | 38.6x, 120x |
| `a2_abs` | 200,000 | 0.030 s | 0.031 s | 38 MiB | 2.174 s | 1,376 MiB | 7.777 s | 746 MiB | 73.0x, 269x |
| `a2_abs` | 400,000 | 0.032 s | 0.048 s | 68 MiB | 5.085 s | 2,651 MiB | 27.443 s | 1,413 MiB | 159x, 782x |
| `a2_abs` | 800,000 | 0.039 s | 0.071 s | 107 MiB | 12.322 s | 4,517 MiB | 70.521 s | 2,498 MiB | 313x, 1,341x |
| `a2_abs_global` | 100,000 | 0.025 s | 0.024 s | 27 MiB | 0.414 s | 338 MiB | 247.218 s | 368 MiB | 16.8x, 10,702x |
| `a2_abs_global` | 200,000 | 0.027 s | 0.028 s | 31 MiB | 0.785 s | 656 MiB | 998.352 s | 696 MiB | 29.5x, 38,696x |
| `a2_abs_global` | 400,000 | 0.032 s | 0.036 s | 46 MiB | 1.556 s | 1,215 MiB | – | – | 48.8x, – |
| `a2_abs_global` | 800,000 | 0.040 s | 0.053 s | 75 MiB | 4.059 s | 2,261 MiB | – | – | 101x, – |

Every A2 query is ahead of both solvers at every size. A2 computes each class's answer once, in an aggregate, and joins
it back to the rows, so every thread works on the rows: `a2` takes 0.026 s on all threads against 0.038 s on one at
800,000 rows, where the solver path folds the query-wide decision into one variable over the same two sums. The absolute
loss puts one helper decision per row in the solver's model; keyed (`a2_abs`) it runs 39 to 313 times Gurobi's speed in
a fortieth of its peak memory at 800,000 rows. Query-wide (`a2_abs_global`) HiGHS took 247 s at 100,000 rows and 998 s
at 200,000, so the sweep stopped running it; that time is inside HiGHS's solve (`../../../issues/bugs.md`, Limitations).