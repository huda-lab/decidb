# Work queue: direct-solve review findings

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `direct_solve/README.md`, `direct_solve/architecture/rules.md`, `direct_solve/framework/done.md`.
Checks: `direct_solve/framework/todo.md` § How to work any task.

## 2. Cleanups (one lane)
- [ ] FRM-13 One copy of the limit-reading code — direct_solve/framework/todo.md § FRM-13

## 3. Error order and constant weights (one lane)
- [ ] FRM-16 Remove R1's solver-order code — direct_solve/framework/todo.md § FRM-16
- [ ] FRM-20 Canonicalizer reads constant coefficients as a count — direct_solve/framework/todo.md § FRM-20

## 4. The ranked-prefix builder (needs: 2, 3) (one lane)
- [ ] FRM-18 Ranked-prefix builder; S3 uses it — direct_solve/framework/todo.md § FRM-18
- [ ] S1-26 S1 uses the ranked-prefix builder — direct_solve/rules/s1_uniform_matroid/todo.md § S1-26 (needs: FRM-18)

## 5. Projection builders (needs: 4) (parallel: S1-28 | R1-20)
- [ ] S1-28 S1 builds projections with the shared stage — direct_solve/rules/s1_uniform_matroid/todo.md § S1-28
- [ ] R1-20 R1 builds projections with the shared stage — direct_solve/rules/r1_continuous_knapsack/todo.md § R1-20
