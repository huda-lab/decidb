# Stage 03 — Logical plan: open work

Open work in what `EXPLAIN` prints. Finished work is in [done.md](done.md) § 6. How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## PLAN-7 — Stages 03 and 04 run from one PlanDecide

- **Problem.** Stages 03 and 04 are orchestrated inline in `Binder::CreatePlan(BoundSelectNode &)`, about 240
  lines.
- **Change.** One `PlanDecide` function runs them; `CreatePlan` calls it.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "plan or canonical"` passes.

## PLAN-8 — The objective's canonical layer is compared as two renderings

- **Problem.** `objective_canonical_differs` (`plan_select_node.cpp`) is found by comparing the objective's text before
  and after canonicalization, while a clause's is reported by the canonicalizer. A rendering that changes without the
  objective changing would print a second layer.
- **Change.** `CanonicalizeObjective` reports "changed", as `CanonicalizeTree` does for a clause, and the comparison of
  two renderings goes.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "explain"` passes with no golden change.
