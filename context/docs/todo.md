# Work queue: clean up N1 and the shared direct-solve code

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `direct_solve/README.md`, `direct_solve/architecture/rules.md`,
`direct_solve/rules/n1_euclidean_ball/done.md`. Checks: `direct_solve/framework/todo.md` § How to work any task.

Each batch finishes completely. Lanes: C3; C4 after C3; C5 after C4; C6 last.

## C3. Refusal messages (one lane)

- [ ] SOL-3 A quadratic-limit refusal names the clause — pipeline/07_solver/todo.md § SOL-3
- [ ] BUG-47 A forced HiGHS says Gurobi is not available — issues/bugs.md § BUG-47
- [ ] BUG-46 A data weight on a square says degree above 2 — issues/bugs.md § BUG-46

## C4. Shared code before N2 (one lane; needs: C3)

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

## Later (not scheduled)

- [ ] UNB-1 Name the runaway decision of a model with a quadratic row — diagnostics/unbounded/todo.md § UNB-1
