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

- **Ruling (owner, 2026-11-06).** Leave the advice out only where `DIAGNOSE` would be refused for lack of a solver,
  decided by the same solver-support check `DIAGNOSE` goes through (`decide_solver_gate.cpp`), so it is one yes or no
  and both paths word the error alike. Where `DIAGNOSE` runs but names no variable, the error keeps the advice; that gap
  is `diagnostics/unbounded/todo.md` § UNB-1.
- **Problem.** `DECIDE optimization is infeasible. Prefix the query with DIAGNOSE to see which clause to change.` and its
  unbounded twin are raised by both paths (`ilp_solver.cpp`, `direct_runtime_checks.cpp`). With no Gurobi, `DIAGNOSE`
  on a query with a quadratic limit is refused ("which needs Gurobi"). With Gurobi, an unbounded quadratic model
  diagnoses as `a non-linear term prevents naming the variable.` (repro: `SUM(POWER(x, 2)) by (k) <= 25` with a NULL `k`
  row, `MAXIMIZE SUM(s * x)`).
- **Change.** A non-throwing form of the solver-support check that says whether a backend can run the query's model.
  The direct path's infeasible and unbounded errors (`DirectInfeasibleErrorPredicate`, `DirectUnboundedErrorPredicate`)
  drop the DIAGNOSE sentence when it says no. The solver path raises these errors only after a backend ran the model,
  so its text keeps the sentence.
- **Ruling (owner, 2026-11-06).** The rule driver decides once per query whether a backend can run it and hands the
  rules the matching error text (a context object), rather than each rule asking for itself; the task runs in the lane
  that edits the four rule files, after a `decidb-architect` review of where that decision is read.
- **Needs.** The model class of the query at rule time: `DeriveDecideModelClass` reads `op.prepared`, which
  `BuildDecidePreparedModel` fills only at physical planning, so the driver reads the IR or the bound constraints instead.
- **Done when.** With no Gurobi licence, `SUM(POWER(x, 2)) <= -1` under `require` raises the infeasible error without the
  DIAGNOSE sentence; with Gurobi, `test_direct_user_facing.py` still compares the two paths word for word; the NULL-key
  repro keeps the sentence.
