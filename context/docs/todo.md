# Work queue: finish A2, then the rest

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `direct_solve/framework/todo.md` § How to work any task, `pipeline/architecture.md`.
Checks: `direct_solve/framework/todo.md` § How to work any task.

Each batch finishes completely. Lanes: T1 and X15 beside
the A batches; B2 after A2; M1 after X15.

## A2. A2's slow all-rows query

- [ ] H-48 A2's group-then-join plan is measured (quiet machine, after the lanes) — direct_solve/framework/todo.md § H-48

## T1. Tests against the oracle

- [ ] TEST-2 Direct-solve arms no test reaches — direct_solve/framework/todo.md § TEST-2

## X15. Other areas' arms no test reaches (one lane per task)

- [ ] BLOAT-27 Canonicalizer and provenance: arms no test reaches — testing/todo.md § BLOAT-27

## B2. The faster plan for the other rules (needs: A2)

- [ ] H-49 N1, S1 and R1 take group-then-join where faster — direct_solve/framework/todo.md § H-49

## M1. Messages quote the query as written (needs: X15)

- [ ] MSG-1 Messages reprint expression trees instead of quoting the source text — issues/code_quality.md § MSG-1

## F1. Formatting (after every other batch)

- [ ] FMT-1 DeciDB sources indented with spaces — issues/code_quality.md § FMT-1
