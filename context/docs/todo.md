# Work queue: S2 prep, then S2 laminar matroid

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `direct_solve/README.md`, `direct_solve/architecture/rules.md`, `direct_solve/framework/done.md`,
`direct_solve/rules/s2_laminar_matroid/definition.md`. Checks: `direct_solve/framework/todo.md` § How to work any task.

## 1. Prep: shared code and the builder (one lane)
- [ ] FRM-22 Group checks under a materialized stage — direct_solve/framework/todo.md § FRM-22
- [ ] FRM-27 Scope proof split, keys compared without order — direct_solve/framework/todo.md § FRM-27
- [ ] FRM-23 One shared linear score — direct_solve/framework/todo.md § FRM-23
- [ ] FRM-24 One reader for count limits — direct_solve/framework/todo.md § FRM-24 (needs: FRM-23)
- [ ] FRM-25 S1 and S3 share count body and fixed rows — direct_solve/framework/todo.md § FRM-25 (needs: FRM-24)
- [ ] FRM-26 Builder ranks over nested levels — direct_solve/framework/todo.md § FRM-26 (needs: FRM-22, FRM-25)

## 2. S2 (needs: 1) (one lane)
- [ ] S2-1 Recognition of nested upper count quotas — direct_solve/rules/s2_laminar_matroid/todo.md § S2-1
- [ ] S2-2 Plan: one ranking per level — direct_solve/rules/s2_laminar_matroid/todo.md § S2-2 (needs: S2-1)

## 3. Measurement (needs: 2)
- [ ] S2-3 S2 against the hand plan and Gurobi — direct_solve/rules/s2_laminar_matroid/todo.md § S2-3
