# Stage 05 — Optimizer: open work

Open work in the optimizer passes. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## OPT-33 — The refusal of a squared objective beside a whole number names what the query holds

- **Problem.** With `DECIDB_FORCE_SOLVER=highs` and `decide_direct_solve='off'`, a squared objective in a model with a
  whole-number column is refused with `squaring a decision that is not REAL is not supported on this installation`
  (`DescribeGap`, `SolverModelClassGap::MIQP`, `decide_solver_gate.cpp`). The text is false where the squared decision
  is REAL and the whole number is something else: `x(REAL), z(INT)` with `MINIMIZE SUM(POWER(x - 1.2, 2))`, or
  `abs(x - 1) >= 0.5` beside that objective, whose exact ABS adds a binary.
- **Decision needed.** The wording (`context/docs/language/errors.md` § solver gap).
- **Recommendation.** `a squared objective beside a whole-number value is not supported on this installation`, at the
  objective: it is true for a declared INT or BOOL decision and for a value a construct adds.
- **Done when.** Both examples give the new text; `./test/decide/run_tests.sh -k "highs_refuses or quadratic"` passes.

## OPT-31 — A square outside a reducer in the objective is refused

- **Problem.** `MINIMIZE power(sum(x) - 3, 2)` (the squared distance of a total from a target) and `MINIMIZE (y -
  3) * (y - 3)` over a query-wide `y` are convex QPs, but the objective's linear form refuses any square outside a
  reducer (`decide_linear_form.cpp`, `'…' multiplies decisions outside a reducer, which is not supported`). The same
  square solves as a constraint (`power(sum(x) - 3, 2) <= 4`), and a product of two query-wide decisions is accepted
  in the objective.
- **Change.** A square of a linear expression outside a reducer becomes a quadratic objective term, as `sum(power(x -
  3, 2))` is: a reducer inside it becomes a helper (the value route), and the solver checks judge convexity and the
  installation as for any quadratic objective.
- **Done when.** Both examples agree with the oracle on Gurobi and HiGHS (continuous); `./test/decide/run_tests.sh -n
  2 -k "quadratic or objective"` passes.
