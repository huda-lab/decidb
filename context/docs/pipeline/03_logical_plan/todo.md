# Stage 03 — Logical plan: open work

Open work in what `EXPLAIN` prints. Finished work is in [done.md](done.md) § 6. How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## PLAN-5 — EXPLAIN names helpers in solver terms

- **Problem.** EXPLAIN prints "an auxiliary variable linearized by McCormick rows" (`decide_source_provenance.cpp`) and
  `abs(ship - 20) becomes an auxiliary variable` beside `sum(ship) by (D.region) becomes a helper decision per group`:
  two words for one thing, and a solver technique, in output for SQL users. The guide quotes the printed text, so it
  keeps them until the binary changes.
- **Change.** EXPLAIN names every helper with the guide's word, a helper, and says what it stands for, not how the
  solver builds it; the guide's quotes follow.
- **Done when.** `grep -rn "auxiliary variable\|McCormick" src` finds no printed string; `./test/decide/run_tests.sh
  -n 2 -k "explain"` passes.

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
