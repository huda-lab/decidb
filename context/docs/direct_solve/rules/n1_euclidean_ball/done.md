# N1 (Euclidean ball) — what works today

Verified on 2026-11-13 at `855e01cf98`: `test/decide/run_direct_matrix.sh` at `116972c7a9`, each of its four suites (auto,
off, serializer, highs) 7,141 passed; C++ `build/release/test/unittest "[decidb]"` 9,649 assertions in 105 cases; the
tests that reach N1 again at `855e01cf98`, which changes only how the plan computes a row's share. Code:
`src/optimizer/decide/direct/n1_euclidean_ball_rule.cpp`. Class definition: [definition.md](definition.md).

## What it admits

One row-scoped `REAL` decision, with:

- **One limit.** One aggregate clause `SUM(c * (s * x)^2) <= B`, read as the canonical square (`DirectWeightedForm`):
  the coefficient `c` folds to one positive constant, and the slope `s` inside the square is a constant or a value of the
  row. Each row's factor is `q = c * s^2`, never negative by form. `POWER(x, 2)`, `x ** 2`, `x * x`, `norm(x, 2)`,
  `SUM(2 * POWER(x, 2))`, `SUM(POWER(2 * x, 2))`, `SUM(POWER(SQRT(w) * x, 2))` and `SUM(POWER(w * x, 2))` all read this
  way, with no case list: a square term whose inner expression is one linear term on the decision, with no constant. A
  constant slope gives a finite `q > 0`. A slope from the data is read and checked on the rows the clause keeps.
- **Scope.** The clause's `when`, the sum's own `when`, a `by` key and a `per K:`, through `DirectProveSharedScope`. A row
  is in a ball when the scope keeps it and its key has no NULL in an element that does not group NULLs.
- **The limit's value.** A constant of magnitude below 1e20, or a value of the source row
  (`DirectIsSourceOnlyNumericMayRaise`). The tightest value in a group applies and a NULL has no limit, so a group with
  no value has no ball. Or an aggregate of the rows (`<= SUM(budget) by (k)`, `<= 0.5 * SUM(l)`), a value of its group
  that the plan computes in a window over the ball's group (`DirectGroupBoundIsReadable`,
  `DirectWindowSink`); a NULL sum has no limit, and a NaN sum raises the solver's error. A sum with its own `when` is
  computed first by the driver, and the rule reads it as a column.
- **The box.** Exactly `[0, inf)` (`x >= 0`, or nothing written) or `(-inf, inf)` (`x >= -1e30`).
- **The objective.** None, or signed `SUM` parts, each with an optional constant factor or divisor and an optional `when`,
  plus an optional objective-level `when`. Terms: a row-valued coefficient times the decision, a per-row constant, and at
  most one square `k * (w * x + t)^2` with a constant `w` and a row-valued `t`, convex in the minimization: `k` is
  positive once the part's sign and factor are applied. A square under a `when`, or under the NULL guard of a nullable
  `t`, leaves some rows without it, so every linear term must be in its part: K is then one constant on every row with
  a linear term. With a slope that differs between rows the objective has no square: the limit is then an ellipsoid,
  whose nearest point to a target is not a scaling of the target.

**The answer.** Each row orients as minimize `K*x^2 + b*x`. Let the row's value be `y = -b / (2*K)` with a square and
`d = -b` without one, after `GREATEST(., 0)` in `[0, inf)`. The row's point `p` is its answer before scaling: `y` with a
square, `d / q` without one (a direction becomes a point through its row's factor, the substitution `z = SQRT(q) * x`
of the [definition](definition.md)). Let `h` be the sum over the ball of the limit's own term at the point, `q * p^2`,
and `r = SQRT(B)` the radius.

| Row | Answer |
|---|---|
| Linear objective | 0 when `h` = 0, otherwise `(p / SQRT(h)) * r` |
| A square | `y` when `SQRT(h) <= r`, otherwise `(p / SQRT(h)) * r` |
| No objective | 0 |
| Outside every ball, with a square | `y` |
| Outside every ball, or with `q` 0 or NULL (a weight of 0 or NULL), without a square | 0 when `d` = 0, otherwise unbounded |

`B < 0` on a row in a ball is infeasible. A NULL coefficient reads as 0 on that row. It raises an error wherever the
solver does: NaN or infinite coefficients, slopes and NaN limits on a row that reads them, a factor `q` past a DOUBLE
(with the solver's message, naming the limit's clause), and any expression that raises on a row that computes it, such as
`SQRT(w)` of a negative weight. Nothing is computed on a row a `when` drops. Every input row is read before one is released, so a
late bad row raises under `LIMIT 1`, `COUNT(*)` or a filter; only `LIMIT 0` skips it. An empty source returns no rows,
whatever the limit says.

**With no Gurobi.** The solver path refuses a quadratic limit on a machine with no Gurobi license ("the SUCH THAT clause
`SUM(POWER(x, 2)) <= 25` squares or multiplies decision variables, which needs Gurobi"). That refusal is raised when the solver plan is built
(`plan_decide.cpp`, `RequireDecideSolverSupport`), after a rule has replaced the DECIDE node, so N1 answers such a query.
A forced backend turns direct solve off, so forced HiGHS still refuses it.

## What it does not admit (falls back to the solver)

A query N1 does not apply to keeps its answer: it runs on the solver, and `require` raises one fixed error.

| Shape | Why |
|---|---|
| Any other bound, even one that does not bind (`x <= 2`, `x <= 50`, `x >= -2`, `x >= 1`, `when f: x >= 0`) | The answer is then the projection onto a clipped ball, which a single scale does not give. |
| A coefficient from the data (`SUM(w * POWER(x, 2))`, `SUM(w * x * x)`) | A negative `w` makes the limit not convex. A weight from the data goes inside the square: `SUM(POWER(SQRT(w) * x, 2))`. |
| A slope from the data in the limit with a square in the objective | The nearest point of an ellipsoid is not a scaling of the target. |
| A ball off center (`SUM(POWER(x - 1, 2)) <= 9`), a constant or a linear term in the limit | Not a ball around zero. |
| `=`, `>=`, `BETWEEN`, two limit clauses, `2 * SUM(...)`, `-SUM(...) >= -25`, `AVG(...)` | Not one plain `<=`. |
| A per-row `POWER(x, 2) <= 9` | Not a sum. |
| A constant limit of magnitude 1e20 or more | The solver stops reading such a right side as a number. |
| `MAXIMIZE` of a convex square, `MINIMIZE` of a concave one, a row-valued slope in the objective's square | Not one K, or not convex. |
| A square on some rows with a linear term elsewhere (`SUM(when f: POWER(x, 2)) + SUM(s * x)`, `SUM(POWER(x - t, 2)) + SUM(s * x)` over a nullable `t`) | K differs between rows with a linear term. |
| `INT`, `BOOL`, a query-wide or entity-scoped decision, two decisions | Not one `REAL` per row. |
| Two squares in an objective | The solver rejects these itself, on both paths. |

## Where it differs from the solver

- **Its own warning.** The divisors are the sum of squares `h`, which the scale divides by, and the objective's square
  factor, which the target `-b / 2K` divides by (`DirectTurningPoint`); the rule is the framework's
  [Number-range warning](../../framework/done.md). `h` is below the normal range at scores of about 1e-154 and below.
  A row whose answer is its own target reads no scale and is silent.
- **More exact row by row.** The formula gives 3 and 4 where Gurobi's barrier stops at 3.0005 and 3.9996, and 0 where it
  stops at 0.001. The objectives agree to within 1e-6 of each other, as the benchmark's agreement check requires.
- **The true answer at any scale.** Multiplying every score by one positive number does not move the maximum, so with a
  limit of 25, scores 3 and 4 times any factor answer 3 and 4. So does a target that far outside the ball. Plain `gurobipy`
  with default parameters returns the same numbers as `off` in this section, so these are Gurobi's and not the model's.
- **Tiny scores.** Gurobi reports optimal with answers that are not: scores 3 and 4 times 1e-6 give 2.69 and 4.20, times
  1e-10 give 1.49 and 1.49, and from 1e-11 down give 0, all with no warning. N1 gives 3 and 4 down to about 1e-154. Below
  that the sum of squares is subnormal and loses precision: at 1e-162 N1 answers 3.018 and 4.024 against a limit of 25,
  which overshoots the ball by 1.2%. Below about 1e-162 `h` reaches 0, and the answer is 0. N1 warns on both and does not
  work around them: its method is correct, and the loss is the DOUBLE's. `off` warns from an objective coefficient of 1e-6.
- **Huge scores.** Gurobi gives 3 and 4 with a warning at 1e12 to 1e25, stops with numerical trouble at 1e30 to 1e50, and
  returns 0 with a warning at 1e100 to 1e153 and above. N1 gives 3 and 4 up to about 1e153. Past about 1e154 the sum of
  squares is past a DOUBLE and N1 raises `DECIDE: MAXIMIZE sum(s * x) computes a number too large for a DOUBLE. Scale its
  data down.`
- **An infinite limit read from the data, with an improving row.** N1 raises unbounded, which the problem is. The solver
  stops with "Gurobi could not solve this model (numerical trouble)".
- **A target from about 1e30.** Gurobi stops with numerical trouble. N1 answers up to the range of the squares, then
  raises the too-large error above.
- **The limit's term past a DOUBLE.** A row's share of `h` is the limit's own term at the row's point, `q * p^2`, so it
  passes a DOUBLE where the user's clause evaluated at that point would, and N1 raises the too-large error. A factor of
  1e-310 under a score of 3 gives a point `3 / q` past a DOUBLE; the solver answers unbounded. A factor of 1e200 with
  targets of 3e60 and 4e60 (`MINIMIZE SUM(POWER(x - s, 2))`) gives a share past a DOUBLE; the solver answers 0.
- **Weights from 1e-150 to 1e150.** N1 gives the unit problem's answer, scaled by `z = SQRT(q) * x`. The solver agrees to
  1e-3 for `SQRT(w) * x` with weights from 1e-3 to 1e3 and `w * x` from 1e-3 to 1. Past that `w * x` at 1e3 warns and is
  0.3% off, and below 1e-10 and above 1e50 the solver answers unbounded or 0.
- **Ties.** With every direction 0, any point in the ball is optimal: N1 returns 0. So does no objective.

## How it works

Group-then-join over two reads of the source, as A2 is (`DirectJoinClassAnswers`): the first read computes each
ball's sum of squared lengths and the values that depend on it once, on the group's one row; the second gives every
row its target and joins its group's row on the key.

The first read:

1. `[W0]`, for a `per K:` a NULL in K can leave with no instance: whether the row's group has one
   (`DirectAddInstanceFlag`).
2. The scope projection: the row's eligibility and its class key (`DirectProjectScope`; the framework's [class
   key](../../framework/done.md)). A row in no ball has a NULL in each column of the key compared with `=`, and no flag
   column stands in the key.
3. `P1`, raw values: every coefficient, constant and limit, and the slope of the limit's square, each on the rows that
   read it (`DirectOnKeptRows`).
4. `P2`: K and b of the row, oriented as a minimum, the row's factor `q = c * s^2`, and the check on every raw value. A
   slope that folds to a constant lays out no slope or `q` column: `q` is a literal in every expression that reads it
   (`Factor`), and DuckDB's rewriter, which the driver runs over the plan, folds those expressions. A constant laid out
   as a column would reach the next projection as a column reference, which the rewriter cannot fold, and with a
   `when`, a `by` key or a `per` the plan masks each raw value to NULL on the rows outside a ball, a CASE over a column
   that no later pass folds. A constant slope cannot raise, so it needs neither the mask nor the check.
5. `[W1]`, for a limit from the data: the tightest limit of the group.
6. `P3`: the target or direction in the domain and the row's point `p`, whether the row is in a ball and whether its
   term is in the ball's sum (`q > 0`), the ball's limit, and the check of `q` past a DOUBLE.
7. `P4`: the row's share `(q * p) * p`, masked to NULL outside the ball's sum, and the checks: the target and its square past a DOUBLE,
   infeasible, unbounded, and with a square the target's precision warning.
8. `H`: a hash aggregate on the group key (`DirectHashAggregate`), one row per group: `h`, the sum of the squared
   lengths; whether every row's checks held (`bool_and`); and the group's limit, for a limit from the data.
9. `C`: a projection over `H`'s output that computes what depends on the group alone, once, on its row: the length
   `sqrt(h)`, whether `h` is below the normal range of a DOUBLE, whether `h` is 0 (with no square), and the verdict of
   the group's checks. This is loop-invariant code motion: the second read would otherwise compute each of them again on
   every row of the group, and a row of a group of 80 offers repeats `sqrt(h)` 80 times. The verdict is a chain in which
   the checks of the rows come first and `h` past a DOUBLE next (`DirectRaiseTooLarge`), so `h`'s own error is raised
   only for a group whose rows passed.

The second read takes `[W0]`, the scope, `P1` and `P2` again and the row's value and point, joins the group's row
from `C` on the class key (a SINGLE join, each column compared by what a NULL means in it, so a row in no ball matches
no group), and ends in `P5`: the source columns, the answer, and the validation column that keeps the verdict and the
warning live. `P5` reads the length, the two flags and the limit from the group's row and computes no `sqrt`,
comparison on `h` or range check of its own.

`H` holds every row of the first read before it gives one, and each check on a row stands below it, so a bad last row
raises under `LIMIT 1`; the target's precision warning is one of them. The second read raises nothing: the first has
raised on any value it would. `h` past a DOUBLE with every square in range is raised in `C`, when the groups are
built, so a group whose `h` is past a DOUBLE raises whether or not a row of it is among the rows the parent reads. That
holds under `LIMIT 1` and `COUNT(*)`, and for one bad group among many. It matches the solver path, which reads every
row and so raises for any group, and the rule that every row is read. Only this plan computes `h`; the solver has no
such number, and the error is the plan's own. The scale warning stays on the row: it prints for a row that reads the
scale with a value of its own (`P5`), and a parent that reads no row of that group does not see it. The framework's
[Number-range warning](../../framework/done.md) gives the rule for the warning. A row outside every ball reads NULL in
`H`'s input, so it adds nothing to the group whose key it shares, and the answer does not read what the join gives it.
The aggregate runs on every thread, which the window it replaces did not (§ Performance).

The plan reads the source twice by itself (`pipelined`), so late materialization adds no third read for a stored
column the parent reads and the rule does not use.

This departs from the standard construction in two ways. A limit read from the data adds `W1`, so that an all-NULL group's
checks can stand below `H`, where the aggregate would have to read its own output. The answer is
`(p / SQRT(h)) * SQRT(B)` and not `SQRT(B / h) * p`: the radius stays in range where `B / h` passes a DOUBLE (a subnormal
`h`). One form serves a constant factor and one per row: `q` is in each row's point and share, not in the radius. The
share is `(q * p) * p`, so `q * p` (about `d` for a direction) is computed first. The unbounded check reads the user's
`B`. The radius is taken in `P5`
only, where every row with `B < 0` has raised. A square's target is `(-0.5 * b) / K`, which stays finite where `2 * K`
does not.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_n1_euclidean_ball.py` (226 tests) | The answers. 108 cases run on the independent oracle (`oracle_solver`, through `add_quadratic_constraint`, built from the raw rows and the groups, limits and coefficients each case states in Python), on the solver path (`off`) and on the direct path (`require`): mixed, nonpositive and zero scores, `B` = 0 and `B` < 0, both domains, projection inside and outside the ball, every limit and objective spelling, `by`, `per K:`, `when`, NULL keys, NULL targets, column limits; a weight inside the square in both spellings, with unequal weights, weights of 0 and NULL, and a constant factor other than 1 beside a square. 5 parent-query contexts. 9 empty-source cases. 17 error cases that must fail on both paths in one class. The boundary table against the solver alone: scores from 1e-200 to 1e154, limits to 1e25, infinite limits, each again under a factor of 2; weights the solver answers. Weights from 1e-150 to 1e150 against the oracle's unit problem, scaled by `z = SQRT(q) * x`. A factor or a share past a DOUBLE. Past the scales the solver answers (a limit of 1e300 over a factor of 1e-10, a subnormal `h`, a square factor of 1e308), the oracle solves the same scores at scale 1 and the direct answer, scaled back, must match it. The warning: `h` in and below the normal range, zero scores, a target in and out of the ball, a row outside every ball, once for many rows, again for each query. A group whose `h` is past a DOUBLE raises under `LIMIT 1` and `COUNT(*)` though no streamed row is in it (5,000 rows, a `LIMIT 1` met by the first vector). Without Gurobi, in a subprocess with an invalid license. |
| `test_direct_rule_conformance.py` | The checks every rule owes, with N1's fixture: schema and rows, all-rows read for 5 kinds of late bad row, serializer, prepared plans, `EXPLAIN` and profiling, `off`, a forced backend (Gurobi, since HiGHS has no quadratic limit), `DIAGNOSE`, 8 near misses that must decline under `require` and answer under `auto` as under `off` (among them a coefficient from the data and a weight beside a square objective), and 7 forms that apply (among them both weighted spellings) |
| `test_direct_user_facing.py` | Every infeasible, unbounded, NaN and infinity error compared in full with the solver's, on 9 shapes, a weighted one among them; the too-large text |
| `test_direct_source_values.py` | Each place N1 reads a value a `when` drops, with infinite, NaN and NULL values and with an expression that raises, the weight inside the square among them |
| C++ `[decidb]` | N1's applicability condition: 36 forms that apply, and a case for each decline branch; no two rules apply to one N1, S1, A1 or R1 shape |

Branch coverage at `73b52ab1d1` (`make decide-coverage`): the tests reach 161 of the rule's 164 arms. The three left are
a limit input with no argument (`COUNT(*)`, two arms) and a square on some rows beside a part with no linear term
(`../../framework/todo.md` § TEST-2). No existing test is pinned to `off`, and none needs to be: logging the rule's
`Apply` over the suite, no test outside this rule's own (`-k "not n1"`) reaches N1 under `auto`. Their quadratic limits
have a bound beside them, so N1 declines, or they force a backend.

The objective tolerance (5e-6 relative) and the ball tolerances (1e-7 on `off`, 1e-14 on `require`) are measured on these
cases and stated in the test file. They are that file's own.

## Performance

Measured on an 11-core Apple M3 Pro: the hand-plan comparison on 2026-11-13 at `2855b9b17f` at a load of about 4.5, the
sweeps against Gurobi on 2026-11-09 at `0ab6224c60` (`n1`, `n1_groups`) and on 2026-11-13 at `035a0a2787` (the weighted
queries).

**Against a hand-written plan.** `benchmark/decide/direct_solve/run.py --compare --sf 1 --queries n1 n1_groups
n1_weighted n1_weighted_groups` runs the
direct plan beside a query written by hand for the same answer (`hand/<query>.sql`), with the same checks and the same
DOUBLE arithmetic, in one process, best of 5, over the 800,000 rows of `partsupp`. CPU is every operator's time summed;
latency is on all threads; the planning DECIDE adds is apart. Both sides gave the same answer.

| Query | Direct CPU | Hand CPU | CPU ratio | Direct latency | Hand latency | DECIDE planning |
|---|---:|---:|---:|---:|---:|---:|
| `n1` | 37.1 ms | 28.8 ms | 1.29 | 8.4 ms | 5.8 ms | 0.32 ms |
| `n1_groups` | 49.2 ms | 41.5 ms | 1.19 | 11.4 ms | 9.2 ms | 0.34 ms |
| `n1_weighted` | 67.0 ms | 49.3 ms | 1.36 | 14.2 ms | 10.2 ms | 0.37 ms |
| `n1_weighted_groups` | 87.2 ms | 62.9 ms | 1.39 | 18.2 ms | 13.6 ms | 0.38 ms |

The hand plans carry the rule's checks. The weighted queries weight each offer's square by its stock,
`SUM(POWER(SQRT(ps_availqty) * ship, 2))`, with `ps_availqty` from 1 to 9,999. The 19 to 39% more CPU is the rule's
general plan against one written for the query. A weighted query computes its factor `q` on every row; a constant slope
makes `q` a literal (`Factor`), which DuckDB's rewriter folds, so `n1` computes no `q`, no `d / q` and no check of `q`.

**Against Gurobi.** `run.py --sf 0.125 0.25 0.5 1 --queries n1 n1_groups`: a fresh `decidb` process per run, median of
3, load 3 to 6. At every size Gurobi's objective agreed with direct solve's within 1e-6. A speedup divides Gurobi's
median by direct's median from the same sweep. Times include process start and database open, about 20 ms, which is most
of the direct time at these sizes.

| Query | Rows | Direct | Direct, 1 thread | Direct peak | Gurobi | Gurobi peak | Speedup |
|---|---:|---:|---:|---:|---:|---:|---:|
| `n1` | 100,000 | 0.030 s | 0.030 s | 32 MiB | 0.421 s | 114 MiB | 13.8x |
| `n1` | 200,000 | 0.030 s | 0.031 s | 33 MiB | 0.874 s | 203 MiB | 29.4x |
| `n1` | 400,000 | 0.029 s | 0.038 s | 36 MiB | 1.697 s | 366 MiB | 59.1x |
| `n1` | 800,000 | 0.032 s | 0.051 s | 40 MiB | 3.295 s | 727 MiB | 102x |
| `n1_groups` | 100,000 | 0.030 s | 0.030 s | 34 MiB | 0.444 s | 123 MiB | 14.6x |
| `n1_groups` | 200,000 | 0.029 s | 0.032 s | 35 MiB | 1.007 s | 205 MiB | 35.1x |
| `n1_groups` | 400,000 | 0.033 s | 0.042 s | 48 MiB | 1.774 s | 419 MiB | 54.4x |
| `n1_groups` | 800,000 | 0.035 s | 0.059 s | 54 MiB | 3.339 s | 803 MiB | 94.1x |

| Query | Rows | Direct | Direct peak | Gurobi | Gurobi peak | Speedup |
|---|---:|---:|---:|---:|---:|---:|
| `n1_weighted` | 100,000 | 0.034 s | 34 MiB | 0.581 s | 115 MiB | 17.2x |
| `n1_weighted` | 200,000 | 0.036 s | 36 MiB | 0.957 s | 206 MiB | 26.5x |
| `n1_weighted` | 400,000 | 0.037 s | 40 MiB | 1.890 s | 371 MiB | 51.3x |
| `n1_weighted` | 800,000 | 0.041 s | 47 MiB | 3.788 s | 736 MiB | 91.8x |
| `n1_weighted_groups` | 100,000 | 0.035 s | 35 MiB | 1.012 s | 123 MiB | 28.9x |
| `n1_weighted_groups` | 200,000 | 0.036 s | 38 MiB | 1.786 s | 215 MiB | 50.0x |
| `n1_weighted_groups` | 400,000 | 0.039 s | 51 MiB | 3.906 s | 423 MiB | 98.9x |
| `n1_weighted_groups` | 800,000 | 0.046 s | 63 MiB | 8.037 s | 812 MiB | 176x |

The weighted rows: `run.py --sf 0.125 0.25 0.5 1 --queries n1_weighted n1_weighted_groups --modes direct gurobi`, median
of 3, load 4.2 at the start; Gurobi's objective agreed with direct solve's within 1e-6 at every size.

Direct time stays near the process's start-up cost up to 800,000 rows, 14 to 176 times Gurobi's speed, in a twelfth to
an eighteenth of its peak memory at 800,000 rows. Direct solve also answers where no Gurobi licence is present.