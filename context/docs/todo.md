# Work queue: finish A2, then the rest

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `direct_solve/framework/todo.md` § How to work any task, `pipeline/architecture.md`.
Checks: `direct_solve/framework/todo.md` § How to work any task.

Each batch finishes completely. Lanes: T1 and M1 side by side; H-50 after T1 (both edit the plan fragments); the A2
measurement on a quiet machine, last.

## A2. A2's slow all-rows query

- [ ] H-50 A direct plan costs two to three times DuckDB's own plan for the same answer (after T1) — direct_solve/framework/todo.md § H-50
- [ ] H-48 A2's group-then-join plan is measured (quiet machine, after H-50 and the lanes) — direct_solve/framework/todo.md § H-48

## T1. Tests against the oracle

- [ ] TEST-2 Direct-solve arms no test reaches — direct_solve/framework/todo.md § TEST-2

## M1. Messages quote the query as written

- [ ] MSG-1 Messages reprint expression trees instead of quoting the source text — issues/code_quality.md § MSG-1

## B1. Outer columns (after M1: both edit the binder)

- [ ] BIND-1 An outer column inside a DECIDE marker leaks the marker's name — pipeline/02_binder/todo.md § BIND-1

## F1. Formatting (after every other batch)

- [ ] FMT-1 DeciDB sources indented with spaces — issues/code_quality.md § FMT-1
