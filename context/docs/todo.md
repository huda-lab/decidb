# Work queue: OPA-rewrites to the deck's one-pass algorithm

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `context/opa_rewrite/README.md`. Checks: `direct_solve/framework/todo.md` § How to
work any task in these todo files.

Phase 0 clears what blocks the real work. Each later phase is one architectural change. Phases run in order; groups
inside a phase run in order; measurement runs fit between phases. `(decision)` marks a task whose **Decision needed**
the owner rules first.

# Phase 0 — Preliminaries

## 0.3 Debug-build run (one lane)
- [ ] TEST-6 One suite run on a debug build — testing/todo.md § TEST-6

# Phase 1 — Errors are raised where they are found

## 1.1 Error-test helper (one lane)
- [ ] TEST-15 One helper asserts an error's kind and location — testing/todo.md § TEST-15

## 1.2 Messages (needs: 1.1) (parallel: EXEC-15, OPT-20 | PARSE-2, PARSE-3 | BIND-2 | OPT-19, FW-4 | PLAN-6, EXEC-19)
- [ ] EXEC-15 Raise the first error reached — pipeline/08_execution/todo.md § EXEC-15
- [ ] OPT-20 An = bound names its group only — pipeline/05_optimizer/todo.md § OPT-20
- [ ] PARSE-2 A removed spelling is a syntax error — pipeline/01_parser/todo.md § PARSE-2
- [ ] PARSE-3 Parse-time messages: kind and where — pipeline/01_parser/todo.md § PARSE-3
- [ ] BIND-2 Bind-time messages: kind and where — pipeline/02_binder/todo.md § BIND-2
- [ ] OPT-19 A NaN message says NaN and where — pipeline/05_optimizer/todo.md § OPT-19
- [ ] FW-4 Direct messages name the clause — direct_solve/framework/todo.md § FW-4 (needs: OPT-19)
- [ ] PLAN-6 Clause text is stored once — pipeline/03_logical_plan/todo.md § PLAN-6
- [ ] EXEC-19 An ordinary query builds no diagnosis state — pipeline/08_execution/todo.md § EXEC-19

## 1.3 Remaining messages (needs: 1.2) (one lane)
- [ ] EXEC-16 Plan- and run-time messages: kind and where — pipeline/08_execution/todo.md § EXEC-16

## 1.4 Error tests (needs: 1.3) (one lane)
- [ ] TEST-11 Solver-path error tests assert kind and location — testing/todo.md § TEST-11
- [ ] TEST-12 Direct-path error tests assert kind and location — testing/todo.md § TEST-12 (needs: TEST-11)

## 1.5 Filed cleanups (one lane)
- [ ] INF-5 Three label formatters no test reaches — diagnostics/infeasible/todo.md § INF-5

# Phase 2 — Direct solve is a separate layer

## 2.1 Bounds and the driver (parallel: FW-5, OPT-21 | FW-6)
- [ ] FW-5 Every bound from side tables — direct_solve/framework/todo.md § FW-5
- [ ] OPT-21 Solver path without direct-solve code — pipeline/05_optimizer/todo.md § OPT-21 (needs: FW-5)
- [ ] FW-6 Two rules that apply to one query (decision) — direct_solve/framework/todo.md § FW-6

## 2.2 Rule plans (needs: 2.1) (one lane)
- [ ] FW-7 Direct rules forward columns by hand (decision) — direct_solve/framework/todo.md § FW-7

# Phase 3 — Each shape and formulation has one owner

## 3.1 Binder and first rewrites (parallel: BIND-4, BIND-5 | OPT-24 | OPT-27)
- [ ] BIND-4 The binder judges algebra by name (decision) — pipeline/02_binder/todo.md § BIND-4
- [ ] BIND-5 Two binder gates for wording — pipeline/02_binder/todo.md § BIND-5 (needs: BIND-4)
- [ ] OPT-24 MIN/MAX clause scoped once per instance — pipeline/05_optimizer/todo.md § OPT-24
- [ ] OPT-27 ABS is lowered in one place — pipeline/05_optimizer/todo.md § OPT-27

## 3.2 MIN/MAX routes and passes (needs: 3.1) (one lane)
- [ ] OPT-25 MIN/MAX inside a sum takes the value route — pipeline/05_optimizer/todo.md § OPT-25
- [ ] OPT-26 Hard single term and flat objective: value route — pipeline/05_optimizer/todo.md § OPT-26
- [ ] OPT-28 One enumerator and one polarity walker — pipeline/05_optimizer/todo.md § OPT-28
- [ ] OPT-29 Node-local rewrites in one traversal — pipeline/05_optimizer/todo.md § OPT-29

## 3.3 Model rows (needs: 3.2) (one lane)
- [ ] FORM-8 One record per kind of model row — pipeline/06_model_formulation/todo.md § FORM-8

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
