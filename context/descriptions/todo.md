# Work queue: perfect R1, starting with its speed

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `08_direct_solve/README.md`, `08_direct_solve/rules/r1_continuous_knapsack/definition.md`,
`08_direct_solve/rules/r1_continuous_knapsack/done.md`. Checks: `08_direct_solve/framework/todo.md` § How to work any
task in these todo files.

## 1. Diagnose: slow by nature or by our plan
- [ ] R1-13 Profile the plan operator by operator — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-13

## 2. Attribute the cost (needs: 1)
- [ ] R1-19 Attribute the cost to plan features — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-19

## 3. Make the plan cheaper (needs: 2)
- [ ] R1-14 Carry only narrow columns through windows — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-14
- [ ] R1-15 Make several decisions no slower than one — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-15

## 4. Owner decision (needs: 3)
- [ ] R1-11 Choose how to beat the solver on one global budget — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-11

## 5. Cleanup (needs: 4)
- [ ] R1-17 Choose how PlanBuilder names its columns — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-17

## 6. Measure last (needs: 5)
- [ ] R1-18 Re-run the performance sweep — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-18
