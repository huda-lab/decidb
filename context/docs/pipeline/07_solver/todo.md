# Stage 07 — Solver: open work

Open work in the backends and their limits. Finished work is in [done.md](done.md).

## SOL-3 — A quadratic-limit refusal names the clause

- **Problem.** With no Gurobi, a query N1 declines because of one extra clause (`x <= 50 AND SUM(POWER(x, 2)) <= 25`)
  is refused with `DECIDE: a SUCH THAT clause squares or multiplies decision variables, which needs Gurobi — not
  available on this machine. Install Gurobi, or rewrite that constraint so each term holds at most one decision
  variable.` (`DescribeGap`, `decide_solver_gate.cpp`). It does not say which clause, and its rewrite would delete the
  sum of squares. The same holds for `x >= -2`, `x >= 1` and `when f: x >= 0`.
- **Change.** Quote the clause as the user wrote it, from its source text, in place of "a SUCH THAT clause". No hint
  about the direct-solve shape (a decline is a plain yes or no).
- **Done when.** The no-Gurobi refusal of the repro names `SUM(POWER(x, 2)) <= 25`; the existing refusal tests read the
  quoted clause.

## SOL-4 — Errors point to DIAGNOSE only where it runs

- **Decision needed.** Whether the infeasible and unbounded errors keep their DIAGNOSE advice where DIAGNOSE cannot
  answer.
- **Problem.** `DECIDE optimization is infeasible. Prefix the query with DIAGNOSE to see which clause to change.` and its
  unbounded twin are raised by both paths (`ilp_solver.cpp`, `direct_runtime_checks.cpp`). With no Gurobi, `DIAGNOSE`
  on a query with a quadratic limit is refused ("which needs Gurobi"). With Gurobi, an unbounded quadratic model
  diagnoses as `a non-linear term prevents naming the variable.` (repro: `SUM(POWER(x, 2)) by (k) <= 25` with a NULL `k`
  row, `MAXIMIZE SUM(s * x)`).
- **Options.** 1. Leave the advice out when DIAGNOSE cannot run on the query (one condition, the same on both paths, so
  the two texts stay identical). 2. Keep the texts and say in `language/guide.md` § 8 that DIAGNOSE needs Gurobi for a
  quadratic model and cannot name the variable of a quadratic unbounded one.
- **Recommendation.** 1.
- **Change.** Per the ruling.
- **Done when.** Each repro raises the agreed text on `off` and `require`; `test_direct_user_facing.py` still compares
  the two paths word for word.
