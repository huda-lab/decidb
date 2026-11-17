# Work queue: close OPA-rewrites' loose ends before Phase 2

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `context/opa_rewrite/handoff.md` § A6, `context/opa_rewrite/merge_notes.md` § 4.1.
Checks: `direct_solve/framework/todo.md` § How to work any task in these todo files.

## 5. Evaluator cleanup (one lane)
- [ ] TEST-3 Cut the dump-format tests, coverage-checked — testing/todo.md § TEST-3
- [ ] EXEC-3 Delete the dead evaluator code — pipeline/08_execution/todo.md § EXEC-3
- [ ] EXEC-4 Debug dumps behind one setting — pipeline/08_execution/todo.md § EXEC-4 (needs: TEST-3)
- [ ] EXEC-5 Release the descent after evaluation — pipeline/08_execution/todo.md § EXEC-5
- [ ] EXEC-6 Comments without the flat evaluator — pipeline/08_execution/todo.md § EXEC-6

## 6. Thread merge (needs: 5) (one lane)
- [ ] EXEC-7 Merge invariants I5 and I6 in release builds — pipeline/08_execution/todo.md § EXEC-7
- [ ] EXEC-8 Real-thread tests for the descent merge — pipeline/08_execution/todo.md § EXEC-8
- [ ] EXEC-9 Sort the 23 descent-split failures — pipeline/08_execution/todo.md § EXEC-9 (needs: EXEC-4)
- [ ] EXEC-10 Collation error names the right row — pipeline/08_execution/todo.md § EXEC-10

## 7. Weak tests (one lane)
- [ ] TEST-4 Tests that check less than their names — testing/todo.md § TEST-4

## 8. Debug-build run (needs: 5–7) (one lane)
- [ ] TEST-6 One suite run on a debug build — testing/todo.md § TEST-6
