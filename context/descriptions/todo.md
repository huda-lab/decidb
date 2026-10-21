# Work queue: decide, measure, simplify, then prune direct-solve tests

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `08_direct_solve/architecture/overview.md`, `08_direct_solve/architecture/rules.md`,
`08_direct_solve/framework/done.md`. Checks: `08_direct_solve/framework/todo.md` § How to work any task.

## 2. S1 on the shared reading
- [ ] H-20 Reducer `WHEN` and empty group in the rewrite — 08_direct_solve/framework/todo.md § H-20

## 3. S1 count bodies (needs: 2)
- [ ] S1-17 Constant terms in the count body — 08_direct_solve/rules/s1_uniform_matroid/todo.md § S1-17

## 4. Toolkit layout
- [ ] H-22 Bound reader holds what R1 needs — 08_direct_solve/framework/todo.md § H-22
- [ ] H-23 One reader for a linear objective — 08_direct_solve/framework/todo.md § H-23
- [ ] H-24 Shared helpers for "true, or raise" — 08_direct_solve/framework/todo.md § H-24
- [ ] S1-12 S1 takes the shared bound reader — 08_direct_solve/rules/s1_uniform_matroid/todo.md § S1-12

## 5. Prune the tests (needs: 3, 4)
- [ ] H-34 Branches no test reaches — 08_direct_solve/framework/todo.md § H-34
- [ ] H-29 Prune the direct-solve tests — 08_direct_solve/framework/todo.md § H-29

## 6. One expression style (needs: 3, 4)
- [ ] H-25 `ExpressionBuilder` is the one way — 08_direct_solve/framework/todo.md § H-25
- [ ] S1-18 S1 builds with `ExpressionBuilder` — 08_direct_solve/rules/s1_uniform_matroid/todo.md § S1-18 (needs: H-25)

## 7. Test tooling for the long run (needs: 4)
- [ ] H-31 Test the decline branches on the IR — 08_direct_solve/framework/todo.md § H-31
- [ ] H-32 Coverage and complexity in the daily checks — 08_direct_solve/framework/todo.md § H-32

## 8. Quick cleanups
- [ ] H-27 Test literals and rule headers — 08_direct_solve/framework/todo.md § H-27

## 9. Blocked
- [ ] H-02 Front end for the new language — 08_direct_solve/framework/todo.md § H-02
