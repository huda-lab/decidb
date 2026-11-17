# Work queue: close OPA-rewrites' loose ends before Phase 2

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `context/opa_rewrite/handoff.md` § A6, `context/opa_rewrite/merge_notes.md` § 4.1.
Checks: `direct_solve/framework/todo.md` § How to work any task in these todo files.

## 4. Crashes and wrong answers on documented syntax (one lane)
- [ ] OPT-8 A square or product whose side holds a reducer raises — pipeline/05_optimizer/todo.md § OPT-8
- [ ] OPT-9 ABS over a nested sum of group sums in the objective — pipeline/05_optimizer/todo.md § OPT-9
- [ ] OPT-10 A product of two reducers in the objective raises — pipeline/05_optimizer/todo.md § OPT-10

## 5. Evaluator cleanup (one lane)
- [ ] TEST-3 Cut the dump-format tests, coverage-checked — testing/todo.md § TEST-3
- [ ] EXEC-3 Delete the dead evaluator code — pipeline/08_execution/todo.md § EXEC-3
- [ ] EXEC-4 Debug dumps behind one setting — pipeline/08_execution/todo.md § EXEC-4 (needs: TEST-3)
- [ ] TEST-7 Port what test_tree_emitter.py's deleted tests checked — testing/todo.md § TEST-7 (needs: TEST-3, EXEC-4)
- [ ] EXEC-5 Release the descent after evaluation — pipeline/08_execution/todo.md § EXEC-5
- [ ] EXEC-6 Comments without the flat evaluator — pipeline/08_execution/todo.md § EXEC-6

## 6. Thread merge (needs: 5) (one lane)
- [ ] EXEC-8 Real-thread tests for the descent merge — pipeline/08_execution/todo.md § EXEC-8
- [ ] EXEC-7 Merge invariants I5 and I6 in release builds — pipeline/08_execution/todo.md § EXEC-7 (needs: EXEC-8)
- [ ] EXEC-9 Sort the 23 descent-split failures — pipeline/08_execution/todo.md § EXEC-9 (needs: EXEC-4)
- [ ] EXEC-10 Collation error names the right row — pipeline/08_execution/todo.md § EXEC-10

## 7. Weak tests (one lane)
- [ ] TEST-4 Tests that check less than their names — testing/todo.md § TEST-4

## 8. Debug-build run (needs: 4–7) (one lane)
- [ ] TEST-6 One suite run on a debug build — testing/todo.md § TEST-6

## 9. Smaller defects (parallel: OPT-11 | PLAN-4 | OPT-12 | EXEC-12 | OPT-13)
- [ ] OPT-11 `per g: x IN (...)` constrains the NULL-key row — pipeline/05_optimizer/todo.md § OPT-11
- [ ] PLAN-4 EXPLAIN's rewrite rows print text the query did not write — pipeline/03_logical_plan/todo.md § PLAN-4
- [ ] OPT-12 A point frame under ABS over infinity answers infeasible — pipeline/05_optimizer/todo.md § OPT-12
- [ ] EXEC-12 The `per` bound disagreement names its group — pipeline/08_execution/todo.md § EXEC-12 (needs: EXEC-3)
- [ ] OPT-13 Decision: fold a `per` over a written column? — pipeline/05_optimizer/todo.md § OPT-13

## 10. Tests (one lane)
- [ ] TEST-8 Two too-large-sum cases run in no mode — testing/todo.md § TEST-8

## 11. Docs and tools (parallel: OPS-3 | EXEC-13, OPS-4 | OPS-5 | PERF-25 | OPS-6 | OPS-7)
- [ ] OPS-3 The OPA handoff and merge notes predate the merge — operations/todo.md § OPS-3
- [ ] EXEC-13 done.md without the flat evaluator — pipeline/08_execution/todo.md § EXEC-13 (needs: EXEC-6)
- [ ] OPS-4 Docs name code that does not exist — operations/todo.md § OPS-4 (needs: EXEC-13)
- [ ] OPS-5 errors.md and the guide quote unprinted text — operations/todo.md § OPS-5
- [ ] PERF-25 The profiler sums spans nothing emits — performance/todo.md § PERF-25
- [ ] OPS-6 Decision: the website's removed spellings — operations/todo.md § OPS-6
- [ ] OPS-7 Docs cite ids no file defines — operations/todo.md § OPS-7
