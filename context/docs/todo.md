# Work queue: finish A2, then the rest

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `direct_solve/framework/todo.md` § How to work any task, `pipeline/architecture.md`.
Checks: `direct_solve/framework/todo.md` § How to work any task.

Each batch finishes completely. Lanes: A1, A2, A3 in order; B1, T1 and X13 beside the A
batches; B2 after A2.

## Owner decisions

- [ ] A2-9 Data weights on the absolute loss — direct_solve/rules/a2_single_facility_location/todo.md § A2-9
- [ ] H-08 VALUE-shape clauses: which rule takes them — direct_solve/framework/todo.md § H-08
- [ ] BLOAT-21 How to trim S1's oracle grid — direct_solve/rules/s1_uniform_matroid/todo.md § BLOAT-21
- [ ] BLOAT-23 Keep or trim the scope combination grid — testing/todo.md § BLOAT-23

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

## T1. Tests against the oracle

- [ ] TEST-1 Expectations computed by hand — testing/todo.md § TEST-1
- [ ] TEST-2 Direct-solve arms no test reaches — direct_solve/framework/todo.md § TEST-2

## X13. Arms no test reaches

- [ ] BLOAT-25 Arms no test reaches in the rest of the DECIDE pipeline — testing/todo.md § BLOAT-25

## B2. The faster plan for the other rules (needs: A2)

- [ ] H-49 N1, S1 and R1 take group-then-join where faster — direct_solve/framework/todo.md § H-49

## Blocked

- [ ] H-06 The two-level objective — direct_solve/framework/todo.md § H-06 (needs: a rule that takes it)
