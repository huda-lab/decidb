# N1 (Euclidean ball) — what works today

Verified on 2026-11-05 at `96aa6186f0`: `make decide-test`, `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`,
`DECIDB_VERIFY_SERIALIZER=1 make decide-test` and `DECIDB_FORCE_SOLVER=highs make decide-test` each 6,169 passed; C++
`build/release/test/unittest "[decidb]"` 3,789 assertions in 81 cases.
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
  no value has no ball. A limit that differs between a group's rows reaches the rule as its group's tightest value; a
  limit that is an aggregate of the rows does not, unless the sum has its own `when`, where the driver computes it first
  and the rule reads a column.
- **The box.** Exactly `[0, inf)` (`x >= 0`, or nothing written) or `(-inf, inf)` (`x >= -1e30`).
- **The objective.** None, or signed `SUM` parts, each with an optional constant factor or divisor and an optional `when`,
  plus an optional objective-level `when`. Terms: a row-valued coefficient times the decision, a per-row constant, and at
  most one square `k * (w * x + t)^2` with a constant `w` and a row-valued `t`, convex in the sense. A square under a
  `when`, or under the NULL guard of a nullable `t`, leaves some rows without it, so every linear term must be in its
  part: K is then one constant on every row with a linear term.

**The answer.** Each row orients as minimize `K*x^2 + b*x`. Let the row's value be `y = -b / (2*K)` with a square and
`d = -b` without one, after `GREATEST(., 0)` in `[0, inf)`, and let `h` be the sum of the squared values over the ball.

| Row | Answer |
|---|---|
| Linear objective | 0 when `h` = 0, otherwise `SQRT(B/q) / SQRT(h) * d` |
| A square | `y` when `h <= B/q`, otherwise `SQRT(B/q) / SQRT(h) * y` |
| No objective | 0 |
| Outside every ball, with a square | `y` |
| Outside every ball, without one | 0 when `d` = 0, otherwise unbounded |

`B < 0` on a row in a ball is infeasible. A NULL coefficient reads as 0 on that row. It raises an error wherever the
solver does: NaN or infinite coefficients and NaN limits on a row that reads them, and any expression that raises on a
row that computes it. Nothing is computed on a row a `when` drops. Every input row is read before one is released, so a
late bad row raises under `LIMIT 1`, `COUNT(*)` or a filter; only `LIMIT 0` skips it. An empty source returns no rows,
whatever the limit says.

**With no Gurobi.** The solver path refuses a quadratic limit on a machine with no Gurobi license ("a SUCH THAT clause
squares or multiplies decision variables, which needs Gurobi"). That refusal is raised when the solver plan is built
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
| `<= SUM(budget) by (k)`, `<= 0.5 * SUM(l)` | The limit is a value of the group's aggregate, left to the plan. |
| A constant limit of magnitude 1e20 or more, or one whose `B / q` leaves the DOUBLE range | The solver stops reading such a right side as a number. |
| `MAXIMIZE` of a convex square, `MINIMIZE` of a concave one, a row-valued slope in the objective's square | Not one K, or not convex. |
| A square on some rows with a linear term elsewhere (`SUM(when f: POWER(x, 2)) + SUM(s * x)`, `SUM(POWER(x - t, 2)) + SUM(s * x)` over a nullable `t`) | K differs between rows with a linear term. |
| `INT`, `BOOL`, a query-wide or entity-scoped decision, two decisions | Not one `REAL` per row. |
| `SUM(weight * POWER(x, 2))`, two squares in an objective | The solver rejects these itself, on both paths. |

## Where it differs from the solver

- **No large-number warning.** A direct plan builds no model, so a number of 1e6 or more prints nothing, where `off` warns.
- **More exact row by row.** The formula gives 3 and 4 where Gurobi's barrier stops at 3.0005 and 3.9996, and 0 where it
  stops at 0.001. The objectives agree to within 1e-6 of each other, as the benchmark's agreement check requires.
- **Tiny scores.** Scores near 1e-100 and 1e-150 answer as scores 3 and 4 do, where Gurobi returns 0. Below about 1e-154
  the sum of squares is subnormal and loses precision: at 1e-162 the answer is 3.018 and 4.024 against a limit of 25, which
  overshoots the ball by 1.2%. Below about 1e-162 `h` reaches 0, and the answer is 0, as Gurobi's is.
- **Huge scores.** Past about 1e154 the sum of squares is past a DOUBLE: N1 raises `DECIDE: MAXIMIZE sum(s * x) computes a
  number too large for a DOUBLE. Scale its data down.`, where Gurobi returns 0 with a warning. From 1e153 Gurobi returns 0
  or stops with numerical trouble; N1 still answers.
- **An infinite limit read from the data, with an improving row.** N1 raises unbounded, which the problem is. The solver
  stops with "Gurobi could not solve this model (numerical trouble)".
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
   infeasible, unbounded.
8. `W2`: `SUM(squared length) OVER (PARTITION BY key)`, `h`. It holds every row before it releases one.
9. `P5`: the source columns, the answer, and the validation column that keeps the checks live.

Each check on a row stands below the last breaker, so a bad last row raises under `LIMIT 1`. The one check above it is `h`
past a DOUBLE with every square in range: only this plan computes that number, the solver has no such error, and a parent
that reads no row of that group sees no error. A row outside every ball reads NULL in the windows' input, so the windows
need not partition on membership; the answer reads what they return only for a row in a ball.

A stored column the parent reads and the rule does not use comes from a second read of the table, joined on the row id
(`direct_late_materialization.cpp`), so `W2` does not hold it.

This departs from the standard construction in two ways. A limit read from the data adds `W1`, so that an all-NULL group's
checks can stand below the last breaker, where one window would have to read its own output. The scale is
`SQRT(B) / SQRT(h)` and not `SQRT(B / h)`, which stays in range where `B / h` does not.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_n1_euclidean_ball.py` (139 tests) | The answers. 95 cases run on the independent oracle (`oracle_solver`, through `add_quadratic_constraint`, built from the raw rows and the groups, limits and coefficients each case states in Python), on the solver path (`off`) and on the direct path (`require`): mixed, nonpositive and zero scores, `B` = 0 and `B` < 0, both domains, projection inside and outside the ball, every limit and objective spelling, `by`, `per K:`, `when`, NULL keys, NULL targets, column limits. 4 parent-query contexts. 9 empty-source cases. 13 error cases that must fail on both paths in one class. The boundary table against the solver alone: scores from 1e-200 to 1e154, limits to 1e25, infinite limits. Without Gurobi, in a subprocess with an invalid license. |
| `test_direct_rule_conformance.py` | The checks every rule owes, with N1's fixture: schema and rows, all-rows read for 5 kinds of late bad row, serializer, prepared plans, `EXPLAIN` and profiling, `off`, forced HiGHS (which refuses on the solver path), `DIAGNOSE`, 7 near misses that must decline under `require` and answer under `auto` as under `off`, and 4 forms that apply |
| `test_direct_user_facing.py` | Every infeasible, unbounded, NaN and infinity error compared in full with the solver's, on 9 shapes; the too-large text |
| `test_direct_source_values.py` | Each place N1 reads a value a `when` drops, with infinite, NaN and NULL values and with an expression that raises |
| C++ `[decidb]` | N1's applicability condition: 27 forms that apply, and a case for each decline branch; no two rules apply to one N1, S1, A1 or R1 shape |

Every decision point of the rule was checked by mutation: with its branch removed, at least one test above fails. The
one exception is a failed read of a square (`DirectReadSquareTerm`): the fold after it declines the same shape, so no
test can tell them apart, and a case only reaches it. No existing test is pinned to `off`: those that now take N1 under `auto` keep their answers, and
`DECIDB_TEST_DIRECT_SOLVE=off` runs them on the solver. `test_many_group_limits_on_squares_solve` forces Gurobi, so it
keeps reaching the solver.

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
| `n1_groups` | 100,000 | 0.027 s | 0.032 s | 52 MiB | 0.447 s | 120 MiB | 16x |
| `n1_groups` | 200,000 | 0.032 s | 0.045 s | 67 MiB | 1.001 s | 210 MiB | 32x |
| `n1_groups` | 400,000 | 0.038 s | 0.070 s | 100 MiB | 1.767 s | 415 MiB | 47x |
| `n1_groups` | 800,000 | 0.049 s | 0.122 s | 185 MiB | 3.349 s | 789 MiB | 68x |

Times include process start and database open, about 20 ms, which is most of the direct time at the small sizes.
