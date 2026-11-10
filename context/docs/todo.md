# Work queue: take O1 out of Prototype

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `direct_solve/rules/o1_monge_assignment/todo.md` § Scope rulings,
`direct_solve/architecture/rules.md`, `direct_solve/rules/o1_monge_assignment/definition.md`.
Checks: each task's **Done when** while working; the full gate in `direct_solve/framework/todo.md` § How to work any
task, once at combine.

## 1. The IR fact (one lane)

- [ ] O1-1 The IR records the two sides of a cross product — direct_solve/rules/o1_monge_assignment/todo.md § O1-1

## 2. The rule and its tests (needs: 1) (parallel: O1-2, O1-3 | O1-4)

- [ ] O1-2 O1's recognition — direct_solve/rules/o1_monge_assignment/todo.md § O1-2
- [ ] O1-3 O1's plan — direct_solve/rules/o1_monge_assignment/todo.md § O1-3 (needs: O1-2)
- [ ] O1-4 O1's standard tests — direct_solve/rules/o1_monge_assignment/todo.md § O1-4

## 3. Docs (needs: 2) (one lane)

- [ ] O1-5 O1's docs leave Prototype — direct_solve/rules/o1_monge_assignment/todo.md § O1-5 (needs: O1-3, O1-4)

## 4. Measurement, quiet machine (needs: 3) (one lane)

- [ ] O1-6 O1's Performance is measured — direct_solve/rules/o1_monge_assignment/todo.md § O1-6

## 5. Optional (needs: 2) (one lane)

- [ ] O1-7 `per l.id` on a primary key — direct_solve/rules/o1_monge_assignment/todo.md § O1-7
