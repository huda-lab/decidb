# Work queue: A2, what is left

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

A2 answers "one shared value closest to every row" without a solver: squared, absolute and maximum absolute losses,
for all rows at once or per key ([rules/a2_single_facility_location/done.md](direct_solve/rules/a2_single_facility_location/done.md)).
It gives the right answers. What is left is one slow query, two open choices, and cleanup of code and tests that grew
larger than they need to be. The report of the work so far is commit `815aac901a`.

Read first: `direct_solve/rules/a2_single_facility_location/done.md`, `direct_solve/architecture/rules.md`. Checks:
`direct_solve/framework/todo.md` § How to work any task.

Each batch finishes completely. Lanes: one at a time (every batch edits A2's rule file), in the order below.

## Owner decisions

- [ ] H-48 Compute A2's answer once per class and join it back — direct_solve/framework/todo.md § H-48
- [ ] A2-9 Data weights on the absolute loss — direct_solve/rules/a2_single_facility_location/todo.md § A2-9

## C1. Code cleanup

- [ ] A2-10 `ABS` is copied into a square's struct — direct_solve/rules/a2_single_facility_location/todo.md § A2-10
- [ ] A2-11 The losses pass their columns as unnamed lists — direct_solve/rules/a2_single_facility_location/todo.md § A2-11

## C2. The slow query (needs the H-48 ruling)

- [ ] H-48 A2's all-rows squared query is slower than the solver — direct_solve/framework/todo.md § H-48

## C3. Test cleanup (after C1 and C2, which change the branches)

- [ ] A2-12 The test grids reach the same branches many times — direct_solve/rules/a2_single_facility_location/todo.md § A2-12

## Set aside (not A2)

- [ ] BUG-51 A bad value under ABS worded as a bound — issues/bugs.md § BUG-51
- [ ] BUG-52 A sum's own when and ABS on a dropped row — issues/bugs.md § BUG-52
