# Stage 06 — Model formulation: open work

Open work in the linearizations this stage emits. Finished work is in [done.md](done.md).

## BLOAT-9 — Bloat in model formulation

- **Problem.** The stage-06 code (`src/decidb/formulation/`; about 4,700 lines) has not been read for bloat.
- **Change.** Remove the bloat by `../../direct_solve/framework/todo.md` § How to work any task (Removing bloat), in
  these files and the tests that reach only what goes.
- **Done when.** `make decide-test` and `build/release/test/unittest "[decidb]"` pass with no answer, error text or
  golden changed, and `DECIDB_FORCE_SOLVER=highs make decide-test`
  passes, and the report lists each removal with its evidence and the lines and `lizard` figures
  before and after.
