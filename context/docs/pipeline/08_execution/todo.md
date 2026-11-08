# Stage 08 — Execution: open work

## BLOAT-11 — Bloat in execution

- **Problem.** The stage-08 code (`src/execution/operator/decide/`, `src/execution/physical_plan/plan_decide.cpp`; about
  4,800 lines) has not been read for bloat.
- **Change.** Remove the bloat by `../../direct_solve/framework/todo.md` § How to work any task (Removing bloat), in
  these files and the tests that reach only what goes.
- **Done when.** `make decide-test` and `build/release/test/unittest "[decidb]"` pass with no answer, error text or
  golden changed, and `DECIDB_VERIFY_SERIALIZER=1 make decide-test`
  passes, and the report lists each removal with its evidence and the lines and `lizard` figures
  before and after.
