# Work queue: close OPA-rewrites' loose ends before Phase 2

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `context/opa_rewrite/handoff.md` § A6, `context/opa_rewrite/merge_notes.md` § 4.1.
Checks: `direct_solve/framework/todo.md` § How to work any task in these todo files.

## 8. Debug-build run (needs: 9, 11) (one lane)
- [ ] TEST-6 One suite run on a debug build — testing/todo.md § TEST-6

## 9. Smaller defects (parallel: OPT-11, OPT-14 | OPT-12, OPT-15 | EXEC-12, PLAN-4)
- [ ] OPT-11 `per g: x IN (...)` constrains the NULL-key row — pipeline/05_optimizer/todo.md § OPT-11
- [ ] PLAN-4 EXPLAIN's rewrite rows print text the query did not write — pipeline/03_logical_plan/todo.md § PLAN-4
- [ ] OPT-12 A value check over a point frame never raises — pipeline/05_optimizer/todo.md § OPT-12
- [ ] EXEC-12 The `per` bound disagreement names its group — pipeline/08_execution/todo.md § EXEC-12
- [ ] OPT-15 A data reducer under ABS in a constraint raises — pipeline/05_optimizer/todo.md § OPT-15
- [ ] OPT-14 Three helpers ask whether an expression holds a reducer — pipeline/05_optimizer/todo.md § OPT-14

## 11. Docs and tools (parallel: OPS-3 | EXEC-13, OPS-4 | OPS-5 | PERF-25 | OPS-7 | EXEC-14)
- [ ] OPS-3 The OPA handoff and merge notes predate the merge — operations/todo.md § OPS-3
- [ ] EXEC-13 done.md without the flat evaluator — pipeline/08_execution/todo.md § EXEC-13
- [ ] OPS-4 Docs name code that does not exist — operations/todo.md § OPS-4 (needs: EXEC-13)
- [ ] OPS-5 errors.md and the guide quote unprinted text — operations/todo.md § OPS-5
- [ ] PERF-25 The profiler sums spans nothing emits — performance/todo.md § PERF-25
- [ ] OPS-7 Docs cite ids no file defines — operations/todo.md § OPS-7
- [ ] EXEC-14 Finalize's comments number phases the code lacks — pipeline/08_execution/todo.md § EXEC-14
