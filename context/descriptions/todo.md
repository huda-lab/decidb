# Work queue: one limit reading, then a leaner direct-solve toolkit

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `08_direct_solve/architecture/overview.md`, `08_direct_solve/architecture/rules.md`,
`08_direct_solve/framework/done.md`. Checks: `08_direct_solve/framework/todo.md` § How to work any task.

## 1. S1 on the shared reading
- [ ] S1-16 Delete S1's own reading of a right-hand aggregate — 08_direct_solve/rules/s1_uniform_matroid/todo.md § S1-16
- [ ] S1-17 Constant terms in the count body — 08_direct_solve/rules/s1_uniform_matroid/todo.md § S1-17 (needs: S1-16)

## 2. The rewrite's remaining cases
- [ ] H-20 Reducer `WHEN` and empty group in the rewrite — 08_direct_solve/framework/todo.md § H-20

## 3. Toolkit layout
- [ ] H-21 Split `direct_plan_factory` by job — 08_direct_solve/framework/todo.md § H-21
- [ ] H-22 Bound reader holds what R1 needs — 08_direct_solve/framework/todo.md § H-22 (needs: H-21)
- [ ] H-23 One reader for a linear objective — 08_direct_solve/framework/todo.md § H-23 (needs: H-21)
- [ ] H-24 Shared helpers for "true, or raise" — 08_direct_solve/framework/todo.md § H-24 (needs: H-21)
- [ ] S1-12 S1 takes the shared bound reader — 08_direct_solve/rules/s1_uniform_matroid/todo.md § S1-12 (needs: H-21)
- [ ] H-26 Driver turns away an unreadable IR — 08_direct_solve/framework/todo.md § H-26

## 4. One expression style (needs: 1, 3)
- [ ] H-25 `ExpressionBuilder` is the one way — 08_direct_solve/framework/todo.md § H-25
- [ ] S1-18 S1 builds with `ExpressionBuilder` — 08_direct_solve/rules/s1_uniform_matroid/todo.md § S1-18 (needs: H-25)

## 5. Quick cleanups
- [ ] H-27 Test literals and rule headers — 08_direct_solve/framework/todo.md § H-27

## 6. Blocked
- [ ] H-02 Front end for the new language — 08_direct_solve/framework/todo.md § H-02
