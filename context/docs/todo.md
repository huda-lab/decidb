# Work queue: OPA-rewrites to the deck's one-pass algorithm

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `context/opa_rewrite/README.md`. Checks: `direct_solve/framework/todo.md` § How to
work any task in these todo files.

Each phase is one architectural change. Phases run in order; groups
inside a phase run in order. `(decision)` marks a task whose **Decision needed**
the owner rules first.

# Phase 6 — The model is written at the nodes

## 6.1 One reading of a reducer body (one lane)
- [ ] MODEL-6 Terms are split from the finished nodes — pipeline/todo.md § MODEL-6

## 6.2 Gate (one lane; needs: 6.1)
- [ ] MODEL-7 Phase 6 gate and docs — pipeline/todo.md § MODEL-7
