# Work queue: direct solve on standard patterns and names

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md` § Design, `08_direct_solve/architecture/overview.md`,
`08_direct_solve/architecture/glossary.md`, `08_direct_solve/architecture/todo.md` (the rename rules and § Decisions
at its top). Checks: `08_direct_solve/rules/_harness/todo.md` § How to work any task.

## 1. Batch A: IR and rule framework, one agent, in order

## 2. Batch B: plan and output, one agent, in order
- [ ] P-12 result boundary becomes an adapter — 08_direct_solve/architecture/todo.md § P-12
- [ ] P-13 decision record becomes the EXPLAIN annotation — 08_direct_solve/architecture/todo.md § P-13 (needs: P-12)
- [ ] P-14 builders, `Calc` and rule-local helpers — 08_direct_solve/architecture/todo.md § P-14

## 3. Class names, run alone (needs: 1, 2)
- [ ] P-21 problem classes take their textbook names — 08_direct_solve/architecture/todo.md § P-21

## 4. Reducer and absorbed bound, run alone
- [ ] P-19 rename across planner and solver — 08_direct_solve/architecture/todo.md § P-19

## 5. Batch C: docs and test files (needs: 1, 2, 3)
- [ ] P-18 test vocabulary in the docs — 08_direct_solve/architecture/todo.md § P-18
- [ ] P-15 shared-engine folder rename, run alone — 08_direct_solve/architecture/todo.md § P-15

## 6. Sweep (needs: every group above)
- [ ] P-17 sweep for retired terms — 08_direct_solve/architecture/todo.md § P-17
