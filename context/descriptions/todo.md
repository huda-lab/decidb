# Work queue: direct solve on standard patterns and names

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md` § Design, `08_direct_solve/architecture/overview.md`,
`08_direct_solve/architecture/todo.md` (the rename rules at its top). Checks: `08_direct_solve/rules/_harness/todo.md`
§ How to work any task.

## 1. Glossary (decisions are made; see P-06 § Change)
- [ ] P-06 one standard name per concept — 08_direct_solve/architecture/todo.md § P-06

## 2. Batch A: IR and rule framework, one agent, in order (needs: 1)
- [ ] P-07 facts layer becomes the IR — 08_direct_solve/architecture/todo.md § P-07
- [ ] P-08 one Prove, a summary instead of Explain — 08_direct_solve/architecture/todo.md § P-08
- [ ] P-09 rule-framework renames — 08_direct_solve/architecture/todo.md § P-09 (needs: P-08)

## 3. Batch B: plan and output, one agent, in order (needs: 1)
- [ ] P-10 validation barrier becomes a pipeline breaker — 08_direct_solve/architecture/todo.md § P-10
- [ ] P-11 second read becomes late materialization — 08_direct_solve/architecture/todo.md § P-11
- [ ] P-12 result boundary becomes an adapter — 08_direct_solve/architecture/todo.md § P-12 (needs: P-10, P-11)
- [ ] P-13 decision record becomes the EXPLAIN annotation — 08_direct_solve/architecture/todo.md § P-13 (needs: P-12)
- [ ] P-14 builders, `Calc` and rule-local helpers — 08_direct_solve/architecture/todo.md § P-14 (needs: P-10)

## 4. Class names, run alone (needs: 2, 3)
- [ ] P-21 problem classes take their textbook names — 08_direct_solve/architecture/todo.md § P-21

## 5. Reducer and absorbed bound, run alone (needs: 1)
- [ ] P-19 rename across planner and solver — 08_direct_solve/architecture/todo.md § P-19

## 6. Batch C: docs and test files (needs: 2, 3, 4)
- [ ] P-18 test vocabulary in the docs — 08_direct_solve/architecture/todo.md § P-18 (orchestrator runs the oracle re-run)
- [ ] P-15 shared-engine folder rename, run alone — 08_direct_solve/architecture/todo.md § P-15

## 7. Sweep (needs: every group above)
- [ ] P-17 sweep for retired terms — 08_direct_solve/architecture/todo.md § P-17
