# Work queue: direct-solve review findings

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `direct_solve/README.md`, `direct_solve/architecture/rules.md`, `direct_solve/framework/done.md`.
Checks: `direct_solve/framework/todo.md` § How to work any task.

## 4. The ranked-prefix builder (one lane)
- [ ] FRM-18 Ranked-prefix builder; S3 uses it — direct_solve/framework/todo.md § FRM-18
- [ ] S1-26 S1 uses the ranked-prefix builder — direct_solve/rules/s1_uniform_matroid/todo.md § S1-26 (needs: FRM-18)

## 5. Projection builders (needs: 4) (parallel: S1-28 | R1-20 | FRM-21)
- [ ] S1-28 S1 builds projections with the shared stage — direct_solve/rules/s1_uniform_matroid/todo.md § S1-28
- [ ] R1-20 R1 builds projections with the shared stage — direct_solve/rules/r1_continuous_knapsack/todo.md § R1-20
- [ ] FRM-21 One forwarding of group-input columns — direct_solve/framework/todo.md § FRM-21

## 6. Owner decision
- [ ] FRM-20 Constant coefficients read as a count: placement — direct_solve/framework/todo.md § FRM-20
