# A1 (separable box-constrained problem) — open work

What A1 does today is in `done.md`; the class is in `definition.md`. How to work and the checks are in
`../../framework/todo.md` § How to work any task.

## BLOAT-4 — Bloat in A1

- **Problem.** A1's rule (`a1_separable_box_rule.cpp`, 560 lines) and its tests (`test_direct_a1_separable_box.py`,
  1,431 lines, 247 cases, and its entry in `_direct_rule_fixtures.py`) have not been read for bloat.
- **Change.** Remove the bloat by `../../framework/todo.md` § How to work any task (Removing bloat), in these files
  only; a duplicate found in shared code is listed for BLOAT-1.
- **Done when.** The matrix passes with no answer, error text or golden changed, and the report lists each removal
  with its evidence and the files' lines, case count and `lizard` figures before and after.
