# R1 — Continuous (fractional) knapsack — open work

R1 splits one shared budget among `REAL` decisions by filling the best value per unit of budget first. The class is in
[definition.md](definition.md); what R1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task. The goal is a plan near the floor that `done.md`
§ Performance measures, with the answers and errors R1 gives today.

## BLOAT-3 — Bloat in R1

- **Problem.** R1's rule (`r1_continuous_knapsack_rule.cpp`, 1,732 lines) and its tests
  (`test_direct_r1_continuous_knapsack.py`, 3,339 lines, 374 cases, and its entry in `_direct_rule_fixtures.py`) have
  not been read for bloat.
- **Change.** Remove the bloat by `../../framework/todo.md` § How to work any task (Removing bloat), in these files
  only; a duplicate found in shared code is listed for BLOAT-1.
- **Done when.** The matrix passes with no answer, error text or golden changed, and the report lists each removal
  with its evidence and the files' lines, case count and `lizard` figures before and after.
