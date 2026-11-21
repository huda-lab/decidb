# Work queue: OPA-rewrites to the deck's one-pass algorithm

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `context/opa_rewrite/README.md`. Checks: `direct_solve/framework/todo.md` § How to
work any task in these todo files.

Each phase is one architectural change. Phases run in order; groups
inside a phase run in order; measurement runs fit between phases. `(decision)` marks a task whose **Decision needed**
the owner rules first.

# Phase 3 — Each shape and formulation has one owner

## 3.2 One route per shape in stage 05 (one lane)
- [ ] OPT-27 ABS is lowered in one place — pipeline/05_optimizer/todo.md § OPT-27
- [ ] OPT-28 One enumerator and one polarity walker — pipeline/05_optimizer/todo.md § OPT-28
- [ ] OPT-29 No rewrite pass reruns without a cause — pipeline/05_optimizer/todo.md § OPT-29

## 3.3 Model rows (needs: 3.2) (one lane)
- [ ] FORM-8 One record per kind of model row — pipeline/06_model_formulation/todo.md § FORM-8

## 3.4 Gaps filed in Phase 3 (needs: 3.2) (one lane)
- [ ] BIND-6 A norm and a range frame refuse a query-wide body — pipeline/02_binder/todo.md § BIND-6
- [ ] OPT-31 A square outside a reducer in the objective — pipeline/05_optimizer/todo.md § OPT-31
- [ ] CANON-1 A SUM or AVG keeps its data addend where its route puts it — pipeline/04_canonicalizer/todo.md § CANON-1
- [ ] EXEC-25 A BETWEEN that constrains nothing warns once — pipeline/08_execution/todo.md § EXEC-25

# Phase 4 — Model building has one contract

## 4.1 Operator and contract (parallel: EXEC-20, EXEC-21, EXEC-22 | EXEC-23 | PLAN-7)
- [ ] EXEC-20 SolverInput is the whole contract — pipeline/08_execution/todo.md § EXEC-20
- [ ] EXEC-21 Solve and route beside the solver — pipeline/08_execution/todo.md § EXEC-21 (needs: EXEC-20)
- [ ] EXEC-22 A plan fact has one home — pipeline/08_execution/todo.md § EXEC-22 (needs: EXEC-20)
- [ ] EXEC-23 The thread model of the descent (decision) — pipeline/08_execution/todo.md § EXEC-23
- [ ] PLAN-7 Stages 03 and 04 run from one PlanDecide — pipeline/03_logical_plan/todo.md § PLAN-7

# Phase 5 — The parser builds the tree

## 5.1 Design (one lane)
- [ ] ARCH-1 Design Phase 5 and file its tasks (decision) — pipeline/todo.md § ARCH-1

# Phase 6 — The model is written at the nodes

## 6.1 Design (needs: Phase 5) (one lane)
- [ ] ARCH-2 Design Phase 6 and file its tasks (decision) — pipeline/todo.md § ARCH-2

# Measurements (outside the phases; confirm each start with the owner)

## M.1 Measurement runs (one lane)
- [ ] PERF-23 Variant numbering — performance/todo.md § PERF-23
- [ ] PERF-24 A nested objective's helpers — performance/todo.md § PERF-24
- [ ] PERF-6 Solver execution — performance/todo.md § PERF-6
- [ ] PERF-8 Memory and scaling — performance/todo.md § PERF-8
- [ ] PERF-1 Methodology, a second host — performance/todo.md § PERF-1
