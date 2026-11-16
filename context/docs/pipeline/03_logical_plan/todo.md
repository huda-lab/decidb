# Stage 03 — Logical plan: open work

Open work in what `EXPLAIN` prints. Finished work is in [done.md](done.md) § 6. How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## PLAN-1 — EXPLAIN prints the rows an ABS or L0 rewrite adds

- **Problem.** `context/docs/language/guide.md` § 9 says EXPLAIN lists the rows the solver receives. For
  `abs(x - 20) <= 5`, the plan prints only `↳ abs(x - 20) becomes an auxiliary variable`, not the envelope rows
  (`abs(x - 20) - x >= -20`). For `norm(x, 0) <= 2`, the count row prints and the indicator links do not. OPA weakened
  the tests to match (handoff B8): `test_explain.py::test_abs_clause_shows_its_source_and_account` asserts the rows
  absent, and cases of `test_scope_explain_diagnose.py::test_explain_accounts_for_each_rewrite` hold `None` where a
  row is due.
- **Change.** Print the rows, read from the tree's F nodes (ruling R4, owner, 2026-11-17). Restore the expectations as
  mainline's versions of both tests (`a50fba79c9`) hold them.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "abs_clause_shows_its_source or explain_accounts_for_each_rewrite"`
  passes with mainline's expectations.

## PLAN-2 — A nested reducer in a constraint gets two contradictory plan lines

- **Problem.** On the query of `context/opa_rewrite/review/09_user_facing_behaviour.md` item D11(b), EXPLAIN ANALYZE
  prints both `↳ sum(per t.h: max(x) by (t.g, t.h)) by (t.g) becomes a helper decision per group` and `... is written
  where it is read, with no column`. Its variants line names the reducer without the outer `by (t.g)`.
- **Change.** One line, stating what the model holds; the variants line names the reducer as written.
- **Done when.** An EXPLAIN ANALYZE test of that query pins the single line and the variants line;
  `./test/decide/run_tests.sh -n 2 -k scope_explain_diagnose` passes.

## PLAN-3 — No variants line for a `per` clause whose bound reads the data

- **Problem.** `per t.g: sum(x) <= t.k * 20` prints no `↳ per t.g: N constraints from M classes` line, where
  `per t.g: y <= t.k + 3` prints one (review 09, item S7).
- **Change.** The variants line prints for every `per` clause that has classes.
- **Done when.** A test pins the line for both clauses; `./test/decide/run_tests.sh -n 2 -k scope_explain_diagnose`
  passes.
