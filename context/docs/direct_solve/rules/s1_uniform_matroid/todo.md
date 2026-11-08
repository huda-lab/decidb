# S1 (uniform-matroid optimization) — open work

What S1 does today is in `done.md`; the class is in `definition.md`. How to work and the checks are in
`../../framework/todo.md` § How to work any task. Every widening also meets the exit gate at the end of this file.

## Exit gate for any S1 widening

An independent proof, behavior tests (the standard tests in `../../architecture/rules.md`), and end-to-end measurement that
does not count timeouts, solver limits or unsupported cases as wins. Finding a relational expression in the catalogue is
not enough.

## BLOAT-2 — Bloat in S1

- **Problem.** S1's rule (`s1_uniform_matroid_rule.cpp`, 1,213 lines) and its tests
  (`test_direct_s1_uniform_matroid.py`, 1,988 lines, 642 cases, and its entry in `_direct_rule_fixtures.py`) have not
  been read for bloat.
- **Change.** Remove the bloat by `../../framework/todo.md` § How to work any task (Removing bloat), in these files
  only; a duplicate found in shared code is listed for BLOAT-1.
- **Done when.** The matrix passes with no answer, error text or golden changed, and the report lists each removal
  with its evidence and the files' lines, case count and `lizard` figures before and after.
