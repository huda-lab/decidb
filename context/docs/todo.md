# Work queue: clean up N1 and the shared direct-solve code

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `direct_solve/README.md`, `direct_solve/architecture/rules.md`,
`direct_solve/rules/n1_euclidean_ball/done.md`. Checks: `direct_solve/framework/todo.md` § How to work any task.

Each batch finishes completely. Lanes: Q1 and C4a side by side; C4b after Q1 and C4a; C5 after C4b; C6 last.

## Q1. Polynomial form (one lane; architect review first)

- [ ] CAN-1 One polynomial form and one shape check after it — pipeline/04_canonicalizer/todo.md § CAN-1

## C4a. Shared code that reads no terms (one lane; beside Q1)

- [ ] H-38 The plan helpers A1, N1 and R1 each copy — direct_solve/framework/todo.md § H-38
- [ ] H-40 `DirectPart::IsPlainSum()` — direct_solve/framework/todo.md § H-40
- [ ] N1-2 A data limit through the shared group-bound path — direct_solve/rules/n1_euclidean_ball/todo.md § N1-2

## C4b. Shared code that reads squares (one lane; needs: Q1, C4a)

- [ ] H-39 One fragment for the separable square objective — direct_solve/framework/todo.md § H-39
- [ ] H-41 The precision warning comes from the shared arithmetic — direct_solve/framework/todo.md § H-41 (needs: H-39)
- [ ] SOL-4 Errors point to DIAGNOSE only where it runs — pipeline/07_solver/todo.md § SOL-4

## C5. Docs (the orchestrator; needs: C4b)

- [ ] H-45 Framework docs describe the shared code as it is — direct_solve/framework/todo.md § H-45
- [ ] N1-5 Guide §10 teaches N1's shape — direct_solve/rules/n1_euclidean_ball/todo.md § N1-5

## C6. Measurement (last; the owner runs it)

- [ ] N1-6 Benchmark sizes past `sf=1` — direct_solve/rules/n1_euclidean_ball/todo.md § N1-6
