# Work queue: close OPA-rewrites' loose ends before Phase 2

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `context/opa_rewrite/handoff.md` § A6, `context/opa_rewrite/merge_notes.md` § 4.1.
Checks: `direct_solve/framework/todo.md` § How to work any task in these todo files.

## 1. Wrong answers (one lane)
- [ ] OPT-6 ABS of a total in the objective read per row — pipeline/05_optimizer/todo.md § OPT-6
- [ ] OPT-7 Constant `per` bound holds on a NULL key — pipeline/05_optimizer/todo.md § OPT-7
- [ ] EXEC-1 "Constrains nothing" warning on a NULL key — pipeline/08_execution/todo.md § EXEC-1 (needs: OPT-7)

## 2. Merge rough edges (parallel: FORM-1, FORM-2 | FORM-3 | TEST-5)
- [ ] FORM-1 Overflowing reducer bound raises TOO_LARGE — pipeline/06_model_formulation/todo.md § FORM-1
- [ ] FORM-2 HiGHS TOO_LARGE names the clause — pipeline/06_model_formulation/todo.md § FORM-2 (needs: FORM-1)
- [ ] FORM-3 Test or delete `close_rounded_box` — pipeline/06_model_formulation/todo.md § FORM-3
- [ ] TEST-5 Session mode fails at once on INTERNAL — testing/todo.md § TEST-5

## 3. Messages and plan text (parallel: PLAN-1, PLAN-2, PLAN-3, FW-1, FW-2 | INF-4, UNB-2 | CANON-1)
- [ ] PLAN-1 EXPLAIN prints ABS and L0 rows — pipeline/03_logical_plan/todo.md § PLAN-1
- [ ] PLAN-2 One plan line for a nest in a constraint — pipeline/03_logical_plan/todo.md § PLAN-2
- [ ] PLAN-3 Variants line for a data-read `per` bound — pipeline/03_logical_plan/todo.md § PLAN-3
- [ ] FW-1 "Objective is 0" warning on the direct path — direct_solve/framework/todo.md § FW-1
- [ ] FW-2 No internal names in direct EXPLAIN ANALYZE — direct_solve/framework/todo.md § FW-2 (needs: FW-1)
- [ ] INF-4 Infeasible DIAGNOSE spells rows and groups as written — diagnostics/infeasible/todo.md § INF-4
- [ ] UNB-2 Unbounded DIAGNOSE spells keys as written — diagnostics/unbounded/todo.md § UNB-2 (needs: INF-4)
- [ ] CANON-1 Span message spells a column once — pipeline/04_canonicalizer/todo.md § CANON-1

## 4. Group named in the value message (needs: 3) (one lane)
- [ ] EXEC-2 "More than one value" names the group — pipeline/08_execution/todo.md § EXEC-2 (needs: INF-4)

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

## 8. Debug-build run (needs: 1–7) (one lane)
- [ ] TEST-6 One suite run on a debug build — testing/todo.md § TEST-6
