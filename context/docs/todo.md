# Work queue: clean up N1 and the shared direct-solve code

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `direct_solve/README.md`, `direct_solve/architecture/rules.md`,
`direct_solve/rules/n1_euclidean_ball/done.md`. Checks: `direct_solve/framework/todo.md` § How to work any task.

Each batch finishes completely. Lanes: C4b; C5 after C4b; C6 last.

## C4b. Shared code that reads squares (one lane)

- [ ] H-39 One fragment for the separable square objective — direct_solve/framework/todo.md § H-39
- [ ] H-41 The precision warning comes from the shared arithmetic — direct_solve/framework/todo.md § H-41 (needs: H-39)
- [ ] SOL-4 Errors point to DIAGNOSE only where it runs — pipeline/07_solver/todo.md § SOL-4

## C5. Docs (the orchestrator; needs: C4b)

- [ ] H-45 Framework docs describe the shared code as it is — direct_solve/framework/todo.md § H-45
- [ ] N1-5 Guide §10 teaches N1's shape — direct_solve/rules/n1_euclidean_ball/todo.md § N1-5

## C6. Measurement (last; the owner runs it)

- [ ] N1-6 Benchmark sizes past `sf=1` — direct_solve/rules/n1_euclidean_ball/todo.md § N1-6
