# Work queue: OPA-rewrites to the deck's one-pass algorithm

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `context/opa_rewrite/README.md`. Checks: `direct_solve/framework/todo.md` § How to
work any task in these todo files.

Each phase is one architectural change. Phases run in order; groups
inside a phase run in order; measurement runs fit between phases. `(decision)` marks a task whose **Decision needed**
the owner rules first.

# Phase 5 — The parser builds the tree

## 5.1 The tree, one carrier at a time (one lane; the tasks run in this order)
- [ ] TREE-1 Parser nodes — pipeline/todo.md § TREE-1
- [ ] TREE-2 `BoundDecide`, moved whole — pipeline/todo.md § TREE-2
- [ ] TREE-3 Clause records — pipeline/todo.md § TREE-3
- [ ] TREE-4 Reducer node and value node — pipeline/todo.md § TREE-4
- [ ] TREE-5 Frame node — pipeline/todo.md § TREE-5
- [ ] TREE-6 Function nodes and one table of helper definitions — pipeline/todo.md § TREE-6
- [ ] TREE-7 One lowering walk; `DecideNode` by kind — pipeline/todo.md § TREE-7
- [ ] TREE-8 Direct rules read `BoundDecide` (decision) — pipeline/todo.md § TREE-8
- [ ] TREE-9 The tag machinery goes — pipeline/todo.md § TREE-9

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
