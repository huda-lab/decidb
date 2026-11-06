# Work queue: direct solve takes A2, single-facility location

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `direct_solve/README.md`, `direct_solve/architecture/rules.md`,
`direct_solve/rules/a2_single_facility_location/definition.md`. Checks: `direct_solve/framework/todo.md` § How to work
any task.

Each batch finishes completely. Lanes: B1 and P1 side by side once H-48 is ruled.

## Owner decisions

- [ ] A2-9 Data weights on the absolute loss — direct_solve/rules/a2_single_facility_location/todo.md § A2-9
- [ ] H-48 Group-then-join for a class's answer — direct_solve/framework/todo.md § H-48

## B1. Solver-path bugs found on the way (independent of A2)

- [ ] BUG-51 A bad value under ABS worded as a bound — issues/bugs.md § BUG-51
- [ ] BUG-52 A sum's own when and ABS on a dropped row — issues/bugs.md § BUG-52

## P1. Plan shape found by the measurement (framework)

- [ ] H-48 A class's answer once per class, not once per row — direct_solve/framework/todo.md § H-48
