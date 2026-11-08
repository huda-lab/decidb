# Stage 02 — Binder: open work

Language-surface work is owned by `../../language/` (the scopes:
`../../language/scopes.md`); what the language does not yet accept is listed in
`../../language/errors.md` §2, and the scope suites in
`../../testing/scope_suite.md`.

## BLOAT-6 — Bloat in the parser, binder and logical plan

- **Problem.** The DECIDE code of stages 01 to 03 (`src/parser/decide/`, `src/planner/expression_binder/decide/`,
  `src/planner/operator/decide/`, `src/planner/decide/decide_source_provenance.cpp`; about 4,500 lines) has not been
  read for bloat.
- **Change.** Remove the bloat by `../../direct_solve/framework/todo.md` § How to work any task (Removing bloat), in
  these files and the tests that reach only what goes.
- **Done when.** `make decide-test` and `build/release/test/unittest "[decidb]"` pass with no answer, error text or
  golden changed, and the report lists each removal with its evidence and the lines and `lizard` figures
  before and after.
