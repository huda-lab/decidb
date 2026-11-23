# Work queue: OPA-rewrites to the deck's one-pass algorithm

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `context/opa_rewrite/README.md`. Checks: `direct_solve/framework/todo.md` § How to
work any task in these todo files.

Each phase is one architectural change. Phases run in order; groups
inside a phase run in order; measurement runs fit between phases. `(decision)` marks a task whose **Decision needed**
the owner rules first.

# Phase 5 — The parser builds the tree

## 5.6 Proof (one lane)
- [ ] TREE-27 The design and the stage docs state the system — pipeline/todo.md § TREE-27
- [ ] TREE-28 The test pass for the phase — pipeline/todo.md § TREE-28 (needs: TREE-27)

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
