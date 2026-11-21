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
