# A1 (independent bounded decisions) — open work

What A1 does today is in `done.md`. The class definition is in `definition.md`. When a task ships, its result moves to
`done.md` and the task is deleted from here. How to run the checks is in `../_harness/todo.md`; read its "How to work any
task" section first. The class definition is met; the one open task is a measurement.

## A1-05 — Refresh the performance table

- **Problem.** The table in `done.md` ("Performance") was measured on 2026-10-06. The solver's bound handling and the shared
  checks have changed since, and the table has not been re-run.
- **Plan.** After `make release`, on a quiet machine, run the two commands under "Reproduce" in `done.md` (under a minute
  in total) and rewrite the table, the 1,000,000-row sentence and the date.
- **Done when.** `done.md` shows numbers from a run you made, with their date. Machine load changes timings, so do not run
  anything else meanwhile.

## Out of the class, no task

`x <> k`, `x IN (...)`, objective `PER`, `AVG`/`MIN`/`MAX` objectives, entity or scalar decisions and coupled decisions are
not A1. They are listed with reasons in `done.md`; several belong to D1 or to other classes.
