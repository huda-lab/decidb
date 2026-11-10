# Work queue: one form per meaning, and O1's `per l.id`

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `pipeline/04_canonicalizer/done.md` § 1, `direct_solve/architecture/rules.md`.
Checks: each task's **Done when**; the full gate in `direct_solve/framework/todo.md` § How to work any task, once at
combine.

## 1. Canonical forms and key facts (parallel: CAN-3, CAN-2 | CAN-4 | O1-7)

- [ ] TST-1 The test CLI tells a NaN from a NULL — testing/todo.md § TST-1
- [ ] CAN-3 The MIN/MAX rewrites read the sign — pipeline/04_canonicalizer/todo.md § CAN-3
- [ ] CAN-2 One objective sense — pipeline/04_canonicalizer/todo.md § CAN-2 (needs: CAN-3)
- [ ] CAN-4 One spelling per function — pipeline/04_canonicalizer/todo.md § CAN-4
- [ ] O1-7 `per l.id` on a key of its table — direct_solve/rules/o1_monge_assignment/todo.md § O1-7

## 2. Follow-ups (parallel: ENG-1 | S1-20 | BND-1)

- [ ] ENG-1 One factor in the direct IR — direct_solve/framework/todo.md § ENG-1 (needs: CAN-2)
- [ ] S1-20 S1 flips a comparison with DuckDB's function — direct_solve/rules/s1_uniform_matroid/todo.md § S1-20
- [ ] BND-1 The two-level objective under a factor or a minus — pipeline/02_binder/todo.md § BND-1 (needs: CAN-3)
