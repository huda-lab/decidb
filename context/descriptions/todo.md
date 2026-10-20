# Work queue: plain declines, then a leaner direct-solve toolkit

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `08_direct_solve/architecture/overview.md`, `08_direct_solve/architecture/rules.md`,
`08_direct_solve/framework/done.md`. Checks: `08_direct_solve/framework/todo.md` § How to work any task.

## 1. Plain yes-or-no declines
- [ ] H-28 A decline is a plain yes or no — 08_direct_solve/framework/todo.md § H-28

## 2. S1 on the shared reading (needs: 1)
- [ ] H-20 Reducer `WHEN` and empty group in the rewrite — 08_direct_solve/framework/todo.md § H-20

## 3. S1 count bodies (needs: 2)
- [ ] S1-17 Constant terms in the count body — 08_direct_solve/rules/s1_uniform_matroid/todo.md § S1-17

## 4. Toolkit layout (needs: 1)
- [ ] H-21 Split `direct_plan_factory` by job — 08_direct_solve/framework/todo.md § H-21
- [ ] H-22 Bound reader holds what R1 needs — 08_direct_solve/framework/todo.md § H-22 (needs: H-21)
- [ ] H-23 One reader for a linear objective — 08_direct_solve/framework/todo.md § H-23 (needs: H-21)
- [ ] H-24 Shared helpers for "true, or raise" — 08_direct_solve/framework/todo.md § H-24 (needs: H-21)
- [ ] S1-12 S1 takes the shared bound reader — 08_direct_solve/rules/s1_uniform_matroid/todo.md § S1-12 (needs: H-21)
- [ ] H-26 Driver turns away an unreadable IR — 08_direct_solve/framework/todo.md § H-26

## 5. One expression style (needs: 2, 4)
- [ ] H-25 `ExpressionBuilder` is the one way — 08_direct_solve/framework/todo.md § H-25
- [ ] S1-18 S1 builds with `ExpressionBuilder` — 08_direct_solve/rules/s1_uniform_matroid/todo.md § S1-18 (needs: H-25)

## 6. Test suite review (needs: 1)
- [ ] H-29 Review the whole test suite for waste — 08_direct_solve/framework/todo.md § H-29

## 7. Quick cleanups
- [ ] H-27 Test literals and rule headers — 08_direct_solve/framework/todo.md § H-27

## 8. Blocked
- [ ] H-02 Front end for the new language — 08_direct_solve/framework/todo.md § H-02
