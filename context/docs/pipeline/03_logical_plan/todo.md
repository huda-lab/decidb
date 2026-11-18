# Stage 03 — Logical plan: open work

Open work in what `EXPLAIN` prints. Finished work is in [done.md](done.md) § 6. How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## PLAN-6 — Clause text is stored once

- **Problem.** `ConstraintSourceInfo` holds written, source, rendered, canonical and spelled text of each side;
  `InitializeConstraintSourceInfo` and `FinalizeConstraintSourceInfo` render on every query; whether the canonical
  form differs is found by comparing two renders. About 45 display strings ride the four model types.
- **Change.** The span id and one canonical rendering; written text is cut from `DecideSourceText` when a message
  needs it; the canonicalizer reports "changed". The diagnosis engines read these fields (26 uses) and change with
  them.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "explain or diagnose or source"` passes.

## PLAN-7 — Stages 03 and 04 run from one PlanDecide

- **Problem.** Stages 03 and 04 are orchestrated inline in `Binder::CreatePlan(BoundSelectNode &)`, about 240
  lines.
- **Change.** One `PlanDecide` function runs them; `CreatePlan` calls it.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "plan or canonical"` passes.
