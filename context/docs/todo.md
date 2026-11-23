# Work queue: OPA-rewrites to the deck's one-pass algorithm

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `context/opa_rewrite/README.md`. Checks: `direct_solve/framework/todo.md` § How to
work any task in these todo files.

Each phase is one architectural change. Phases run in order; groups
inside a phase run in order; measurement runs fit between phases. `(decision)` marks a task whose **Decision needed**
the owner rules first.

# Phase 5 — The parser builds the tree

## 5.1 The reducer node (one lane)
- [ ] TREE-10 A reducer holds its kind; a norm is bound as itself — pipeline/todo.md § TREE-10
- [ ] TREE-11 Reducer equality and hash hold every fact — pipeline/todo.md § TREE-11 (needs: TREE-10)
- [ ] TREE-12 Member tests are a reducer field — pipeline/todo.md § TREE-12 (needs: TREE-11)
- [ ] TREE-13 A frame is its own class — pipeline/todo.md § TREE-13 (needs: TREE-12)
- [ ] TREE-14 The side copy of an inlined reducer (decision) — pipeline/todo.md § TREE-14 (needs: TREE-13)
- [ ] TREE-15 The written name's home (decision) — pipeline/todo.md § TREE-15

## 5.2 The clause record (needs: 5.1) (one lane)
- [ ] TREE-16 A clause's shape has one writer — pipeline/todo.md § TREE-16
- [ ] TREE-17 The body enumeration re-canonicalizes what a pass marks — pipeline/todo.md § TREE-17 (needs: TREE-16)
- [ ] TREE-18 The bound-read debug check states its condition — pipeline/todo.md § TREE-18 (needs: TREE-17)
- [ ] TREE-19 Clause-record leftovers — pipeline/todo.md § TREE-19
- [ ] TREE-20 A recorded reducer's query-wide fact comes from its record — pipeline/todo.md § TREE-20

## 5.3 The helper definitions (needs: 5.2) (one lane)
- [ ] TREE-21 A definition class per kind — pipeline/todo.md § TREE-21
- [ ] TREE-22 A definition owns its rows and its facts — pipeline/todo.md § TREE-22 (needs: TREE-21)

## 5.4 The tree (needs: 5.3) (one lane)
- [ ] TREE-7 Each atom of a clause is handed over with its node — pipeline/todo.md § TREE-7
- [ ] TREE-23 `DecideNode` holds no copy; a G names its operand — pipeline/todo.md § TREE-23 (needs: TREE-7)
- [ ] TREE-24 `DecideNode` is a class per kind — pipeline/todo.md § TREE-24 (needs: TREE-23)
- [ ] TREE-25 Terms split from the finished nodes (decision) — pipeline/todo.md § TREE-25 (needs: TREE-24)

## 5.5 Parser, direct rules, tags (needs: 5.4) (one lane)
- [ ] TREE-26 The parser's reducer node holds its call; one AND splitter — pipeline/todo.md § TREE-26
- [ ] TREE-8 Direct rules read `BoundDecide` (decision) — pipeline/todo.md § TREE-8
- [ ] TREE-9 The tag machinery goes — pipeline/todo.md § TREE-9 (needs: TREE-8)

## 5.6 Proof (needs: 5.5) (one lane)
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
