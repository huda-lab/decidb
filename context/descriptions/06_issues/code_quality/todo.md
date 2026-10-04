# Code Quality Issues — Open

Code-quality issues include duplication, dead code, fragile patterns, unclear
naming, missing coverage, and diagnostic-quality defects that do not change the
ordinary optimization result. Solve-correctness bugs belong in `../bugs/todo.md`.

Each entry: short title, location (`file:line`), what's wrong, why it matters,
and when/during which task it was discovered.

- **Unreachable "A reducer takes one BY (...)" refusal.**
  `src/planner/expression_binder/decide/decide_binder.cpp` (`BindReducerBy`):
  the grammar already rejects a second `BY` with a bare syntax error, so the
  binder's dedicated message never fires. Either route the grammar to it (an
  error production `decide_reducer decide_by_keys`) or delete it. Found by the
  reducers probe of the 2026-09-29 syntax review.
- **`decide_declared_before_from` is set but no longer read.** The duplicate
  declaration check moved to `makeDecideClause` (2026-09-30); the flag in
  `gramparse.hpp` / `select.y`'s `decide_declaration` action can go.
