# Stage 07 — Solver: open work

Open work in the backends and their limits. Finished work is in [done.md](done.md).

## SOL-4 — Errors point to DIAGNOSE only where it runs

- **Ruling (owner, 2026-11-06).** Leave the advice out only where `DIAGNOSE` would be refused for lack of a solver,
  decided by the same solver-support check `DIAGNOSE` goes through (`decide_solver_gate.cpp`), so it is one yes or no
  and both paths word the error alike. Where `DIAGNOSE` runs but names no variable, the error keeps the advice; that gap
  is `diagnostics/unbounded/todo.md` § UNB-1.
- **Reviewed design (architect, 2026-11-06).**
  - One message source: `DecideInfeasibleMessage(bool diagnose_runs)` and `DecideUnboundedMessage(bool diagnose_runs)`
    in `decide_value_messages.hpp`.
  - A non-throwing `FindDecideSolverGap(needed, backend)` and `DecideSolverRuns(needed)` sit beside the throwing gate.
  - The direct driver builds `DirectStateErrors {infeasible, unbounded}` once, after a rule is selected, from
    `DecideSolverRuns(ClassifyDecideModel(ir.model_shape))`, and passes it to `Apply`. The texts reach run time as the
    constants inside `error(...)`.
  - The direct reader reports less when unsure, which keeps the advice.
  - The classifier and the shape readers are built by CAN-1 (`04_canonicalizer/todo.md` § CAN-1, ruling f).
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
