# N1 (Euclidean ball) — what works today

Verified on 2026-11-06, on top of `eae1714ed5` (the code of `5f6fde4753`): `test/decide/run_direct_matrix.sh`, each of
its four suites (auto, off, serializer, highs) 6,293 passed; C++ `build/release/test/unittest "[decidb]"` 3,876
assertions in 82 cases.
`test/decide/run_direct_matrix.sh` renews these numbers in one command (about 3 minutes; it prints one line per check).
Code: `src/optimizer/decide/direct/n1_euclidean_ball_rule.cpp`. Class definition: [definition.md](definition.md).

## What it admits

One row-scoped `REAL` decision, with:

- **One limit.** One aggregate clause `SUM(q * x^2) <= B`, whose factor `q` folds to one positive constant. `POWER(x, 2)`,
  `x ** 2`, `x * x`, `norm(x, 2)`, `SUM(2 * POWER(x, 2))` and `SUM(POWER(2 * x, 2))` all read this way, with no case list:
  a square term whose inner expression is one linear term on the decision, with no constant. The ball is
  `SUM(x^2) <= B / q`.
- **Scope.** The clause's `when`, the sum's own `when`, a `by` key and a `per K:`, through `DirectProveSharedScope`. A row
  is in a ball when the scope keeps it and its key has no NULL in an element that does not group NULLs.
- **The limit's value.** A constant of magnitude below 1e20, or a value of the source row
  (`DirectIsSourceOnlyNumericMayRaise`). The tightest value in a group applies and a NULL has no limit, so a group with
  no value has no ball. Or an aggregate of the rows (`<= SUM(budget) by (k)`, `<= 0.5 * SUM(l)`), a value of its group
  that the plan computes in a window over the ball's group, as S1 and R1 do (`DirectGroupBoundIsReadable`,
  `DirectWindowSink`); a NULL sum has no limit, and a NaN sum raises the solver's error. A sum with its own `when` is
  computed first by the driver, and the rule reads it as a column.
- **The box.** Exactly `[0, inf)` (`x >= 0`, or nothing written) or `(-inf, inf)` (`x >= -1e30`).
- **The objective.** None, or signed `SUM` parts, each with an optional constant factor or divisor and an optional `when`,
  plus an optional objective-level `when`. Terms: a row-valued coefficient times the decision, a per-row constant, and at
  most one square `k * (w * x + t)^2` with a constant `w` and a row-valued `t`, convex in the sense. A square under a
  `when`, or under the NULL guard of a nullable `t`, leaves some rows without it, so every linear term must be in its
  part: K is then one constant on every row with a linear term.

**The answer.** Each row orients as minimize `K*x^2 + b*x`. Let the row's value be `y = -b / (2*K)` with a square and
`d = -b` without one, after `GREATEST(., 0)` in `[0, inf)`, let `h` be the sum of the squared values over the ball, and
let `r = SQRT(B) / SQRT(q)` be its radius.

| Row | Answer |
|---|---|
| Linear objective | 0 when `h` = 0, otherwise `(d / SQRT(h)) * r` |
| A square | `y` when `SQRT(h) <= r`, otherwise `(y / SQRT(h)) * r` |
| No objective | 0 |
| Outside every ball, with a square | `y` |
| Outside every ball, without one | 0 when `d` = 0, otherwise unbounded |

`B < 0` on a row in a ball is infeasible. A NULL coefficient reads as 0 on that row. It raises an error wherever the
solver does: NaN or infinite coefficients and NaN limits on a row that reads them, and any expression that raises on a
row that computes it. Nothing is computed on a row a `when` drops. Every input row is read before one is released, so a
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
| A row-valued factor (`SUM(POWER(w * x, 2))`) | Not a ball. This is N2. |
| A ball off center (`SUM(POWER(x - 1, 2)) <= 9`), a constant or a linear term in the limit | Not a ball around zero. |
| `=`, `>=`, `BETWEEN`, two limit clauses, `2 * SUM(...)`, `-SUM(...) >= -25`, `AVG(...)` | Not one plain `<=`. |
| A per-row `POWER(x, 2) <= 9` | Not a sum. |
| A constant limit of magnitude 1e20 or more | The solver stops reading such a right side as a number. |
| `MAXIMIZE` of a convex square, `MINIMIZE` of a concave one, a row-valued slope in the objective's square | Not one K, or not convex. |
| A square on some rows with a linear term elsewhere (`SUM(when f: POWER(x, 2)) + SUM(s * x)`, `SUM(POWER(x - t, 2)) + SUM(s * x)` over a nullable `t`) | K differs between rows with a linear term. |
| `INT`, `BOOL`, a query-wide or entity-scoped decision, two decisions | Not one `REAL` per row. |
| `SUM(weight * POWER(x, 2))`, two squares in an objective | The solver rejects these itself, on both paths. |

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
- **A subnormal factor under a huge limit.** A constant `q` below the normal range of a DOUBLE with a limit near the top
  of one gives a radius past a DOUBLE, and the answer reads `inf` or NaN, as DuckDB's own arithmetic does. Every
  realistic factor keeps the radius in range.
- **Ties.** With every direction 0, any point in the ball is optimal: N1 returns 0. So does no objective.

## How it works

One read of the source, and one pipeline breaker for a limit that is a constant:

1. `[W0]`, for a `per K:` a NULL in K can leave with no instance: whether the clause has one (`DirectAddInstanceFlag`).
2. The scope projection: the row's membership and key (`DirectProjectScope`).
3. `P1`, raw values: every coefficient, constant and limit, each on the rows that read it (`DirectOnKeptRows`).
4. `P2`: K and b of the row, oriented as a minimum, and the check on every raw value.
5. `[W1]`, for a limit from the data: the tightest limit of the group.
6. `P3`: the target or direction in the domain, whether the row is in a ball, and the ball's limit.
7. `P4`: the row's squared length, masked to NULL outside a ball, and the checks: the target and its square past a DOUBLE,
   infeasible, unbounded, and with a square the target's precision warning.
8. `W2`: `SUM(squared length) OVER (PARTITION BY key)`, `h`. It holds every row before it releases one.
9. `P5`: the source columns, the answer, and the validation column that keeps the checks and the warning live.

Each check on a row stands below the last breaker, so a bad last row raises under `LIMIT 1`; the target's precision
warning is one of them. Two checks stand above it: `h` past a DOUBLE with every square in range, and the scale warning
from `h`. Only this plan computes `h`, and a parent that reads no row of that group sees neither. The solver has no
such error, and the framework's [Number-range warning](../../framework/done.md) gives the rule for the warning. A row
outside every ball reads NULL in the windows' input, so the windows need not partition on membership; the answer reads
what they return only for a row in a ball.

A stored column the parent reads and the rule does not use comes from a second read of the table, joined on the row id
(`direct_late_materialization.cpp`), so `W2` does not hold it.

This departs from the standard construction in two ways. A limit read from the data adds `W1`, so that an all-NULL group's
checks can stand below the last breaker, where one window would have to read its own output. The answer is
`(value / SQRT(h)) * (SQRT(B) / SQRT(q))` and not `SQRT(B / (q * h)) * value`: the first factor is at most 1 and the
radius stays in range where `B / q` or `B / h` passes a DOUBLE (a limit of 1e300 over a factor of 1e-10, or a subnormal
`h`). The unbounded check reads the user's `B`, so a finite `B` over a small `q` is a ball. The radius is taken in `P5`
only, where every row with `B < 0` has raised. A square's target is `(-0.5 * b) / K`, which stays finite where `2 * K`
does not.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_n1_euclidean_ball.py` (147 tests) | The answers. 83 cases run on the independent oracle (`oracle_solver`, through `add_quadratic_constraint`, built from the raw rows and the groups, limits and coefficients each case states in Python), on the solver path (`off`) and on the direct path (`require`): mixed, nonpositive and zero scores, `B` = 0 and `B` < 0, both domains, projection inside and outside the ball, every limit and objective spelling, `by`, `per K:`, `when`, NULL keys, NULL targets, column limits. 4 parent-query contexts. 9 empty-source cases. 13 error cases that must fail on both paths in one class. The boundary table against the solver alone: scores from 1e-200 to 1e154, limits to 1e25, infinite limits. Past the scales the solver answers (a limit of 1e300 over a factor of 1e-10, a subnormal `h`, a square factor of 1e308), the oracle solves the same scores at scale 1 and the direct answer, scaled back, must match it. The warning: `h` in and below the normal range, zero scores, a target in and out of the ball, a row outside every ball, once for many rows, again for each query. Without Gurobi, in a subprocess with an invalid license. |
| `test_direct_rule_conformance.py` | The checks every rule owes, with N1's fixture: schema and rows, all-rows read for 5 kinds of late bad row, serializer, prepared plans, `EXPLAIN` and profiling, `off`, a forced backend (Gurobi, since HiGHS has no quadratic limit), `DIAGNOSE`, 7 near misses that must decline under `require` and answer under `auto` as under `off`, and 4 forms that apply |
| `test_direct_user_facing.py` | Every infeasible, unbounded, NaN and infinity error compared in full with the solver's, on 9 shapes; the too-large text |
| `test_direct_source_values.py` | Each place N1 reads a value a `when` drops, with infinite, NaN and NULL values and with an expression that raises |
| C++ `[decidb]` | N1's applicability condition: 28 forms that apply, and a case for each decline branch; no two rules apply to one N1, S1, A1 or R1 shape |

Every decision point of the rule was checked by mutation: with its branch removed, at least one test above fails. No
existing test is pinned to `off`, and none needs to be: logging the rule's `Apply` over the suite, none of the 5,962
tests outside this rule's own (`-k "not n1"`) reaches N1 under `auto`. Their quadratic limits have a bound beside them, so
N1 declines, or they force a backend.

The objective tolerance (5e-6 relative) and the ball tolerances (1e-7 on `off`, 1e-14 on `require`) are measured on these
cases and stated in the test file. They are that file's own.

## Performance

Measured with `benchmark/decide/direct_solve/run.py --sf 0.125 0.25 0.5 1 --queries n1 n1_groups` on 2026-11-05 at
`d5a5559a49` (median of 3 runs, load about 8). Every size agreed with Gurobi on the objective within 1e-6, and no solver
run failed. `n1` has one limit over every row; `n1_groups` has one per supplier.

| Query | Decisions | Direct | Direct, 1 thread | Direct peak | Gurobi | Gurobi peak | Speedup |
|---|---:|---:|---:|---:|---:|---:|---:|
| `n1` | 100,000 | 0.027 s | 0.026 s | 28 MiB | 0.417 s | 113 MiB | 15x |
| `n1` | 200,000 | 0.034 s | 0.033 s | 33 MiB | 0.857 s | 202 MiB | 25x |
| `n1` | 400,000 | 0.047 s | 0.047 s | 42 MiB | 1.656 s | 363 MiB | 35x |
| `n1` | 800,000 | 0.080 s | 0.079 s | 60 MiB | 3.483 s | 722 MiB | 44x |
| `n1` | 1,600,000 | 0.147 s | 0.147 s | 104 MiB | 6.234 s | 1,409 MiB | 43x |
| `n1` | 3,200,000 | 0.270 s | 0.271 s | 175 MiB | 13.212 s | 2,808 MiB | 49x |
| `n1` | 6,400,000 | 0.513 s | 0.511 s | 316 MiB | 25.699 s | 4,732 MiB | 50x |
| `n1_groups` | 100,000 | 0.027 s | 0.032 s | 52 MiB | 0.447 s | 120 MiB | 16x |
| `n1_groups` | 200,000 | 0.032 s | 0.045 s | 67 MiB | 1.001 s | 210 MiB | 32x |
| `n1_groups` | 400,000 | 0.038 s | 0.070 s | 100 MiB | 1.767 s | 415 MiB | 47x |
| `n1_groups` | 800,000 | 0.049 s | 0.122 s | 185 MiB | 3.349 s | 789 MiB | 68x |
| `n1_groups` | 1,600,000 | 0.078 s | 0.242 s | 349 MiB | 6.712 s | 1,575 MiB | 87x |
| `n1_groups` | 3,200,000 | 0.127 s | 0.461 s | 541 MiB | 60.338 s | 3,104 MiB | 474x |
| `n1_groups` | 6,400,000 | 0.207 s | 0.907 s | 909 MiB | 32.837 s | 4,817 MiB | 159x |

Times include process start and database open, about 20 ms, which is most of the direct time at the small sizes.

The sizes from 1,600,000 decisions up (`--sf 2 4 8`) were measured on 2026-11-06 at `f39828c659`, load about 3 to 4,
with the same agreement check; every size agreed. Direct time grows linearly with the rows and stays under a second at
6,400,000 decisions, with a fifteenth (`n1`) to a fifth (`n1_groups`) of Gurobi's peak memory. Gurobi's time on
`n1_groups` at 3,200,000 decisions spread from 17.3 s to 89.6 s over its three runs, with model build steady at 0.26 s,
so the spread is the solver's; its median is above its time at 6,400,000.
