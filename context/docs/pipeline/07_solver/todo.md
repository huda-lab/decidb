# Stage 07 — Solver: open work

Open work in the backends and their limits. Finished work is in [done.md](done.md).

## BLOAT-10 — Bloat in the solver backends

- **Problem.** The stage-07 code (`src/decidb/solver/`, `src/decidb/gurobi/`, `src/decidb/naive/`; about 2,600 lines)
  has not been read for bloat.
- **Change.** Remove the bloat by `../../direct_solve/framework/todo.md` § How to work any task (Removing bloat), in
  these files and the tests that reach only what goes.
- **Done when.** `make decide-test` and `build/release/test/unittest "[decidb]"` pass with no answer, error text or
  golden changed, and `DECIDB_FORCE_SOLVER=highs make decide-test`
  passes, and the report lists each removal with its evidence and the lines and `lizard` figures
  before and after.
