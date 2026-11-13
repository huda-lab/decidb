# Work queue: build direct-solve rule R4 (max-min fairness)

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: direct_solve/rules/r4_max_min_fairness/definition.md, direct_solve/architecture/rules.md,
direct_solve/rules/n1_euclidean_ball/done.md. Checks: direct_solve/framework/todo.md § How to work any task.

## 1. Build R4 (one lane)
- [ ] R4-1 Plain rule, constant positive weight — direct_solve/rules/r4_max_min_fairness/todo.md § R4-1
- [ ] R4-2 Weights of any sign — direct_solve/rules/r4_max_min_fairness/todo.md § R4-2 (needs: R4-1)
- [ ] R4-3 Budget per group, `when` on the budget — direct_solve/rules/r4_max_min_fairness/todo.md § R4-3 (needs: R4-2)
- [ ] R4-4 Decision shared by key — direct_solve/rules/r4_max_min_fairness/todo.md § R4-4 (needs: R4-3)

## 2. Measure (one lane) (needs: 1)
- [ ] R4-5 Benchmark against hand plan and Gurobi — direct_solve/rules/r4_max_min_fairness/todo.md § R4-5
