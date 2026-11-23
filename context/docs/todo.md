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

## 6.3 Instances, not input rows (one lane)
- [ ] MODEL-22 A frame read is resolved where its term is written — pipeline/todo.md § MODEL-22

## 6.4 The emitter's structure (one lane; needs: 6.3)
- [ ] MODEL-17 The emitter reads a term in one place — pipeline/todo.md § MODEL-17
- [ ] MODEL-18 Working columns apart from the result; emitter fills the solver input — pipeline/todo.md § MODEL-18 (needs: MODEL-17)

## 6.5 Layering (one lane)
- [ ] MODEL-19 Stage 06 has one driver; the AVG under `<>` is the optimizer's — pipeline/todo.md § MODEL-19
- [ ] MODEL-20 The helper-form rule names the readers that take members — pipeline/todo.md § MODEL-20
- [ ] MODEL-21 One term extraction for a clause and the objective — pipeline/todo.md § MODEL-21

## 6.6 Gate (one lane; needs: 6.3 to 6.5)
- [ ] MODEL-7 Phase 6 gate and docs — pipeline/todo.md § MODEL-7
