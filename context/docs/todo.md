# Work queue: remove bloat, then finish A2, then the rest

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `direct_solve/framework/todo.md` § How to work any task, `pipeline/architecture.md`.
Checks: `direct_solve/framework/todo.md` § How to work any task.

Each batch finishes completely. Lanes: X9 and X10 side by side; X11 after them; then A1,
A2, A3 in order; B1 beside the A batches; B2 after A2.

## Owner decisions

- [ ] A2-9 Data weights on the absolute loss — direct_solve/rules/a2_single_facility_location/todo.md § A2-9
- [ ] H-08 VALUE-shape clauses: which rule takes them — direct_solve/framework/todo.md § H-08

## X9. Bloat: scope and constraint tests

- [ ] BLOAT-13 Bloat in the scope tests — testing/todo.md § BLOAT-13
- [ ] BLOAT-14 Bloat in the constraint tests — testing/todo.md § BLOAT-14

## X10. Bloat: nonlinear, error and end-to-end tests

- [ ] BLOAT-15 Bloat in the nonlinear and reducer tests — testing/todo.md § BLOAT-15
- [ ] BLOAT-16 Bloat in the error, output and end-to-end tests — testing/todo.md § BLOAT-16

## X11. Bloat: docs, by the orchestrator (needs: X9, X10)

- [ ] BLOAT-17 Bloat in the direct-solve docs — operations/todo.md § BLOAT-17
- [ ] BLOAT-18 Bloat in the pipeline docs — operations/todo.md § BLOAT-18
- [ ] BLOAT-19 Bloat in the other docs — operations/todo.md § BLOAT-19

## A1. A2 code cleanup

- [ ] A2-10 `ABS` is copied into a square's struct — direct_solve/rules/a2_single_facility_location/todo.md § A2-10
- [ ] A2-11 The losses pass their columns as unnamed lists — direct_solve/rules/a2_single_facility_location/todo.md § A2-11

## A2. A2's slow all-rows query (needs: A1; H-48 is a decision, asked first)

- [ ] H-48 A2's answer once per class, joined back — direct_solve/framework/todo.md § H-48

## A3. A2 test trim (needs: A2)

- [ ] A2-12 The test grids reach the same branches many times — direct_solve/rules/a2_single_facility_location/todo.md § A2-12

## B1. Solver-path bugs

- [ ] BUG-51 A bad value under ABS worded as a bound — pipeline/05_optimizer/todo.md § BUG-51
- [ ] BUG-52 A sum's own when and ABS on a dropped row — pipeline/05_optimizer/todo.md § BUG-52

## B2. The faster plan for the other rules (needs: A2)

- [ ] H-49 N1, S1 and R1 take group-then-join where faster — direct_solve/framework/todo.md § H-49

## Blocked

- [ ] H-06 The two-level objective — direct_solve/framework/todo.md § H-06 (needs: a rule that takes it)
- [ ] BLOAT-21 S1 grid cases that share one plan — direct_solve/rules/s1_uniform_matroid/todo.md § BLOAT-21 (needs: a coverage pass the owner asks for)
