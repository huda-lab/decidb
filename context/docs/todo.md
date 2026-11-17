# Work queue: close OPA-rewrites' loose ends before Phase 2

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `context/opa_rewrite/handoff.md` § A6, `context/opa_rewrite/merge_notes.md` § 4.1.
Checks: `direct_solve/framework/todo.md` § How to work any task in these todo files.

## 6. Thread merge (one lane)
- [ ] EXEC-8 Real-thread tests for the descent merge — pipeline/08_execution/todo.md § EXEC-8
- [ ] EXEC-7 Merge invariants I5 and I6 in release builds — pipeline/08_execution/todo.md § EXEC-7 (needs: EXEC-8)
- [ ] EXEC-9 Sort the 23 descent-split failures — pipeline/08_execution/todo.md § EXEC-9
- [ ] EXEC-10 Collation error names the right row — pipeline/08_execution/todo.md § EXEC-10

## 7. Weak tests (one lane)
- [ ] TEST-4 Tests that check less than their names — testing/todo.md § TEST-4

## 8. Debug-build run (needs: 6–7) (one lane)
- [ ] TEST-6 One suite run on a debug build — testing/todo.md § TEST-6

## 9. Smaller defects (parallel: OPT-11 | PLAN-4 | OPT-12 | EXEC-12 | OPT-13 | OPT-15 | OPT-14)
- [ ] OPT-11 `per g: x IN (...)` constrains the NULL-key row — pipeline/05_optimizer/todo.md § OPT-11
- [ ] PLAN-4 EXPLAIN's rewrite rows print text the query did not write — pipeline/03_logical_plan/todo.md § PLAN-4
- [ ] OPT-12 A value check over a point frame never raises — pipeline/05_optimizer/todo.md § OPT-12
- [ ] EXEC-12 The `per` bound disagreement names its group — pipeline/08_execution/todo.md § EXEC-12
- [ ] OPT-13 Decision: fold a `per` over a written column? — pipeline/05_optimizer/todo.md § OPT-13
- [ ] OPT-15 A data reducer under ABS in a constraint raises — pipeline/05_optimizer/todo.md § OPT-15
- [ ] OPT-14 Three helpers ask whether an expression holds a reducer — pipeline/05_optimizer/todo.md § OPT-14

## 10. Tests (one lane)
- [ ] TEST-8 Two too-large-sum cases run in no mode — testing/todo.md § TEST-8
- [ ] TEST-9 The coverage run fails the invalidated-session test — testing/todo.md § TEST-9

## 11. Docs and tools (parallel: OPS-3 | EXEC-13, OPS-4 | OPS-5 | PERF-25 | OPS-7 | EXEC-14)
- [ ] OPS-3 The OPA handoff and merge notes predate the merge — operations/todo.md § OPS-3
- [ ] EXEC-13 done.md without the flat evaluator — pipeline/08_execution/todo.md § EXEC-13
- [ ] OPS-4 Docs name code that does not exist — operations/todo.md § OPS-4 (needs: EXEC-13)
- [ ] OPS-5 errors.md and the guide quote unprinted text — operations/todo.md § OPS-5
- [ ] PERF-25 The profiler sums spans nothing emits — performance/todo.md § PERF-25
- [ ] OPS-7 Docs cite ids no file defines — operations/todo.md § OPS-7
- [ ] EXEC-14 Finalize's comments number phases the code lacks — pipeline/08_execution/todo.md § EXEC-14
