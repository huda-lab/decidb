# Stage 04 — Canonicalizer: open work

Open work in the shape boundary (`ClassifyScoped`, `PeelScale` and the product form). Finished work is in
[done.md](done.md).

## BLOAT-7 — Bloat in the canonicalizer

- **Problem.** The stage-04 code (`src/planner/decide/` apart from `decide_source_provenance.cpp`; about 3,900 lines)
  has not been read for bloat.
- **Change.** Remove the bloat by `../../direct_solve/framework/todo.md` § How to work any task (Removing bloat), in
  these files and the tests that reach only what goes.
- **Done when.** `make decide-test` and `build/release/test/unittest "[decidb]"` pass with no answer, error text or
  golden changed, and the report lists each removal with its evidence and the lines and `lizard` figures
  before and after.
