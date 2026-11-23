# Work queue: OPA-rewrites to the deck's one-pass algorithm

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `context/opa_rewrite/README.md`. Checks: `direct_solve/framework/todo.md` § How to
work any task in these todo files.

Each phase is one architectural change. Phases run in order; groups
inside a phase run in order. `(decision)` marks a task whose **Decision needed**
the owner rules first.

# Phase 6 — The model is written at the nodes

## 6.0 Owner decisions
- [ ] MODEL-6 One reading of a reducer body (decision) — pipeline/todo.md § MODEL-6
- [ ] MODEL-8 The helper of a link whose data may split its classes (decision) — pipeline/todo.md § MODEL-8
- [ ] MODEL-24 The owner of a clause's emitted rows (decision) — pipeline/todo.md § MODEL-24

## 6.5 Layering (one lane)
- [ ] MODEL-20 The helper-form rule names the readers that take members — pipeline/todo.md § MODEL-20
- [ ] MODEL-21 One term extraction for a clause and the objective — pipeline/todo.md § MODEL-21

## 6.6 Gate (one lane; needs: 6.5)
- [ ] MODEL-7 Phase 6 gate and docs — pipeline/todo.md § MODEL-7
