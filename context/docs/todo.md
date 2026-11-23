# Work queue: OPA-rewrites to the deck's one-pass algorithm

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `context/opa_rewrite/README.md`. Checks: `direct_solve/framework/todo.md` § How to
work any task in these todo files.

Each phase is one architectural change. Phases run in order; groups
inside a phase run in order; measurement runs fit between phases. `(decision)` marks a task whose **Decision needed**
the owner rules first.

# Phase 6 — The model is written at the nodes

## 6.1 The helper's form is the optimizer's (one lane)
- [ ] MODEL-1 The optimizer sets each reducer helper's form — pipeline/todo.md § MODEL-1
- [ ] MODEL-2 A G hands its members to its reader — pipeline/todo.md § MODEL-2 (needs: MODEL-1)

## 6.2 A record per group (one lane; needs: 6.1)
- [ ] MODEL-3 Stage 06 reads a clause through its emitted rows — pipeline/todo.md § MODEL-3
- [ ] MODEL-4 An aggregate root writes a record per group — pipeline/todo.md § MODEL-4 (needs: MODEL-3)
- [ ] MODEL-5 A per-row root writes a record per kept row — pipeline/todo.md § MODEL-5 (needs: MODEL-4)

## 6.3 One reading of a reducer body (one lane; needs: 6.2)
- [ ] MODEL-6 Terms are split from the finished nodes — pipeline/todo.md § MODEL-6

## 6.4 Gate (one lane; needs: 6.3)
- [ ] MODEL-7 Phase 6 gate and docs — pipeline/todo.md § MODEL-7

# Measurements (outside the phases; confirm each start with the owner)

## M.1 Measurement runs (one lane)
- [ ] PERF-23 Variant numbering — performance/todo.md § PERF-23
- [ ] PERF-24 A nested objective's helpers — performance/todo.md § PERF-24
- [ ] PERF-6 Solver execution — performance/todo.md § PERF-6
- [ ] PERF-8 Memory and scaling — performance/todo.md § PERF-8
- [ ] PERF-1 Methodology, a second host — performance/todo.md § PERF-1
