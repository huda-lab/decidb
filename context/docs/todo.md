# Work queue: OPA-rewrites to the deck's one-pass algorithm

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `context/opa_rewrite/README.md`. Checks: `direct_solve/framework/todo.md` § How to
work any task in these todo files.

Each phase is one architectural change. Phases run in order; groups
inside a phase run in order; measurement runs fit between phases. `(decision)` marks a task whose **Decision needed**
the owner rules first.

# Phase 1 — Errors are raised where they are found

## 1.2 Messages left (one lane)
- [ ] EXEC-19 A model row carries a back-pointer, and the diagnosis derives the rest (decision) — pipeline/08_execution/todo.md § EXEC-19

## 1.4 Error tests (one lane)
- [ ] TEST-11 Solver-path error tests assert kind and location — testing/todo.md § TEST-11
- [ ] TEST-12 Direct-path error tests assert kind and location — testing/todo.md § TEST-12 (needs: TEST-11)

## 1.5 Filed cleanups (parallel: PARSE-4, BIND-6 | PLAN-8, PLAN-10 | OPT-30, OPT-31, OPT-32 | FW-8 | TEST-16, TEST-17 | PERF-27, EXEC-25 | INF-5)
- [ ] INF-5 Three label formatters no test reaches — diagnostics/infeasible/todo.md § INF-5
- [ ] PARSE-4 Two span roles of a BETWEEN are never read — pipeline/01_parser/todo.md § PARSE-4
- [ ] BIND-6 Two binder messages name the wrong fragment — pipeline/02_binder/todo.md § BIND-6
- [ ] PLAN-8 The objective's canonical layer is compared as two renderings — pipeline/03_logical_plan/todo.md § PLAN-8
- [ ] PLAN-10 Check names and comments say "message" — pipeline/03_logical_plan/todo.md § PLAN-10
- [ ] OPT-30 Two value checks of one comparison — pipeline/05_optimizer/todo.md § OPT-30
- [ ] OPT-31 A node built with no value keeps no location — pipeline/05_optimizer/todo.md § OPT-31
- [ ] OPT-32 Optimizer labels are built on every query — pipeline/05_optimizer/todo.md § OPT-32
- [ ] FW-8 A square's NaN is named differently on the two paths — direct_solve/framework/todo.md § FW-8
- [ ] TEST-16 A frame reducer reads no row in no class — testing/todo.md § TEST-16
- [ ] TEST-17 Branches of the new messages no test reaches — testing/todo.md § TEST-17
- [ ] PERF-27 The pipeline profiler's error filter misses a refusal — performance/todo.md § PERF-27
- [ ] EXEC-25 A failing coefficient is found at the first entry (decision) — pipeline/08_execution/todo.md § EXEC-25

## 1.6 Filed after EXEC-19 (needs: 1.2) (one lane)
- [ ] PLAN-9 `source_columns` is built and serialized on every query — pipeline/03_logical_plan/todo.md § PLAN-9

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

## 5.2 Filed Phase 5 tasks (needs: ARCH-1) (one lane)
- [ ] ARCH-3 A node's caret location and its identity as the user's text are two things — pipeline/todo.md § ARCH-3

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

# Open questions (owner)

- **The `DECIDE:` prefix of the number-range warning** (`decide_numeric_policy.hpp`). The warning no longer tells the
  user what to change; whether its prefix follows V1 of the Phase 1 plan is open.
