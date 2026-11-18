# Work queue: close OPA-rewrites' loose ends before Phase 2

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`. What follows this queue: `context/opa_rewrite/README.md`.
Checks: `direct_solve/framework/todo.md` § How to work any task in these todo files.

## 1. Docs and tools (parallel: EXEC-13, OPS-4 | OPS-5 | PERF-25 | OPS-7 | EXEC-14)
- [ ] EXEC-13 done.md without the flat evaluator — pipeline/08_execution/todo.md § EXEC-13
- [ ] OPS-4 Docs name code that does not exist — operations/todo.md § OPS-4 (needs: EXEC-13)
- [ ] OPS-5 errors.md and the guide quote unprinted text — operations/todo.md § OPS-5
- [ ] PERF-25 The profiler sums spans nothing emits — performance/todo.md § PERF-25
- [ ] OPS-7 Docs cite ids no file defines — operations/todo.md § OPS-7
- [ ] EXEC-14 Finalize's comments number phases the code lacks — pipeline/08_execution/todo.md § EXEC-14

## 2. Defects found in phase 7 (parallel: OPT-18 | OPT-17, OPT-16)
- [ ] OPT-18 An INT under ABS reads `avg(t.id)` per row — pipeline/05_optimizer/todo.md § OPT-18
- [ ] OPT-17 Solver-path checks strip the bound's tags — pipeline/05_optimizer/todo.md § OPT-17
- [ ] OPT-16 A frame's message suggests a refused `when` — pipeline/05_optimizer/todo.md § OPT-16

## 3. Debug-build run (needs: 1, 2) (one lane)
- [ ] TEST-6 One suite run on a debug build — testing/todo.md § TEST-6
