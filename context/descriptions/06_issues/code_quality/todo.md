# Code Quality Issues — Open

Code-quality issues include duplication, dead code, fragile patterns, unclear
naming, missing coverage, and diagnostic-quality defects that do not change the
ordinary optimization result. Solve-correctness bugs belong in `../bugs/todo.md`.

Each entry: short title, location (`file:line`), what's wrong, why it matters,
and when/during which task it was discovered.

## Two fixed bug shapes have no regression test

- **Location:** `test/decide/tests/test_scope_bare_clauses.py` (the first) and
  `test/decide/tests/test_scope_explain_diagnose.py` (the second).
- **What's wrong:** two logged bugs stopped reproducing once every clause followed the
  scope rules, but no test pins the answer:
  - `x <= 10 AND MAX(x) <= MAX(v)` over `(1, 'b', 9), (2, 'b', 1), (3, 'a', 2) t(id, g, v)`
    answers `[9, 9, 9]`; it raised an INTERNAL error at term extraction.
  - DIAGNOSE of `DECIDE per t.g: x(INT) SUCH THAT x >= 15 AND x <= 20 AND MIN(x) <= 12`
    (also `per (): min(x) <= 12`) suggests `min(x) <= 15`, as the per-row `x(INT)` does;
    it answered `undiagnosed`.
- **Why it matters:** a regression would go unnoticed; the crash guard of
  `test/decide/decidb_cli.py` sees an INTERNAL error only in a query some test runs.
- **Discovered:** 2026-10-19, re-running every open entry of `../bugs/todo.md` for ANR
  item 7 (`09_anr_language/done.md`).

## A clause's scope is carried in parallel copies

- **Location:** `src/include/duckdb/planner/decide/decide_prepared_model.hpp`
  (`DecideConstraint`: `when_condition`, `group_key`, `per_key`, `class_key`) and
  `src/include/duckdb/planner/operator/decide/logical_decide.hpp`
  (`ComposedMinMaxConstraint`: `when`, `per_key`, `class_key`); the walkers
  `EnterClauseScope` (`decide_optimizer.cpp`, used by the ABS, norm/IN and value
  rewrites) and the canonicalizer's `DecideEnclosingScope`, beside the path of a nested
  reducer (`DecideReducerPath`, which both read); the rule that a query-wide
  decision beside the reducers counts once, applied by hand in `ilp_model_builder.cpp`
  (linear and quadratic paths) and `linearization_not_equal.cpp`, beside execution's
  `CountOnce` (`physical_decide.cpp`).
- **What's wrong:** one concept, several owners. A clause's `when` / `per K:` / `by`
  scope is declared on two structs and filled by two readers; the enclosing scope is
  walked by two walkers; the once-count rule has several hand copies.
- **Why it matters:** a change to how a clause is scoped, rendered or counted must find
  every copy; a missed one gives a wrong count or an echo that differs between EXPLAIN
  and the rewrite accounts.
- **Work (D47):** one `DecideClauseScope { when; per_key; class_key; group_key; }`
  embedded in both structs, one enclosing-scope walker in `decide_constraint_walk.hpp`
  that yields a reducer's path too, and one owner of the once-count rule at execution.
  Behaviour does not change: the scope suites, the golden corpus (identical on both
  solvers) and `DECIDB_VERIFY_SERIALIZER=1` stay green. The renames that go with it
  (D46) are `../../03_expressivity/scope/todo.md`.
- **Discovered:** 2026-10-18, removing the spellings the scope language replaced
  (`../../09_anr_language/done.md`, item 7).
