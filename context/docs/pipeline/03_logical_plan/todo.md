# Stage 03 — Logical plan: open work

Open work in what `EXPLAIN` prints. Finished work is in [done.md](done.md) § 6. How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## PLAN-7 — Stages 03 and 04 run from one PlanDecide

- **Problem.** Stages 03 and 04 are orchestrated inline in `Binder::CreatePlan(BoundSelectNode &)`, about 240
  lines.
- **Change.** One `PlanDecide` function runs them; `CreatePlan` calls it.
- **Done when.** `./test/decide/run_tests.sh -k "plan or canonical"` passes.
