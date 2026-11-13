# Work queue: widen N1 to the ellipsoid support function

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: direct_solve/rules/n1_euclidean_ball/definition.md, direct_solve/rules/n1_euclidean_ball/done.md,
direct_solve/architecture/rules.md. Checks: direct_solve/framework/todo.md § How to work any task.

## 1. Class docs and recognition (parallel: N1-7 | N1-8)
- [ ] N1-7 Fold the ellipsoid support function into N1's class — direct_solve/rules/n1_euclidean_ball/todo.md § N1-7
- [ ] N1-8 Recognition: a row-valued slope inside the square — direct_solve/rules/n1_euclidean_ball/todo.md § N1-8

## 2. Plan and tests (one lane) (needs: 1)
- [ ] N1-9 Plan: the factor per row in N1's two reads — direct_solve/rules/n1_euclidean_ball/todo.md § N1-9 (needs: N1-7, N1-8)
- [ ] N1-10 Standard suites and numeric boundaries for the weighted form — direct_solve/rules/n1_euclidean_ball/todo.md § N1-10 (needs: N1-9)

## 3. Measurement (one lane) (needs: 2)
- [ ] N1-11 Measure the weighted form against the hand plan and Gurobi — direct_solve/rules/n1_euclidean_ball/todo.md § N1-11 (needs: N1-9)
