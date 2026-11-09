# Work queue: finish A2, then the rest

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `direct_solve/framework/todo.md` § How to work any task, `pipeline/architecture.md`.
Checks: `direct_solve/framework/todo.md` § How to work any task.

Each batch finishes completely. Lanes: C1; the A2 measurement on a quiet machine, last.

## A2. A2's slow all-rows query

- [ ] H-48 A2's group-then-join plan is measured (quiet machine) — direct_solve/framework/todo.md § H-48

## C1. DuckDB's own tests (one lane, in order)

- [ ] COMPAT-1 DeciDB cannot open a database file whose views DuckDB stored — issues/bugs.md § COMPAT-1
- [ ] COMPAT-2 The `BOOL` keyword quotes DuckDB's names in messages — issues/bugs.md § COMPAT-2
- [ ] COMPAT-3 A list comprehension over subscripted names reaches the binder — issues/bugs.md § COMPAT-3
- [ ] COMPAT-4 `concurrent_append_metadata_queries` fails intermittently — issues/bugs.md § COMPAT-4
