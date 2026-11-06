# Work queue: clean up N1 and the shared direct-solve code

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `direct_solve/README.md`, `direct_solve/architecture/rules.md`,
`direct_solve/rules/n1_euclidean_ball/done.md`. Checks: `direct_solve/framework/todo.md` § How to work any task.

Each batch finishes completely. Lanes: Q1 and U1 side by side; C4 after Q1; C5 after C4; C6 last.

## Q1. Polynomial form (one lane; architect review first)

- [ ] CAN-1 One polynomial form for decision products — pipeline/04_canonicalizer/todo.md § CAN-1

## U1. Unbounded diagnosis (one lane)

- [ ] UNB-1 Name the runaway decision of a model with a non-linear part — diagnostics/unbounded/todo.md § UNB-1

## C4. Shared code before N2 (one lane; needs: Q1)

- [ ] H-38 The plan helpers A1, N1 and R1 each copy — direct_solve/framework/todo.md § H-38
- [ ] H-40 `DirectPart::IsPlainSum()` — direct_solve/framework/todo.md § H-40
- [ ] H-39 One fragment for the separable square objective — direct_solve/framework/todo.md § H-39 (needs: H-38)
- [ ] H-41 The precision warning comes from the shared arithmetic — direct_solve/framework/todo.md § H-41 (needs: H-39)
- [ ] N1-2 A data limit through the shared group-bound path — direct_solve/rules/n1_euclidean_ball/todo.md § N1-2
- [ ] SOL-4 Errors point to DIAGNOSE only where it runs — pipeline/07_solver/todo.md § SOL-4

## C5. Docs (the orchestrator; needs: C4)

- [ ] H-45 Framework docs describe the shared code as it is — direct_solve/framework/todo.md § H-45
- [ ] N1-5 Guide §10 teaches N1's shape — direct_solve/rules/n1_euclidean_ball/todo.md § N1-5

## C6. Measurement (last; the owner runs it)

- [ ] N1-6 Benchmark sizes past `sf=1` — direct_solve/rules/n1_euclidean_ball/todo.md § N1-6
