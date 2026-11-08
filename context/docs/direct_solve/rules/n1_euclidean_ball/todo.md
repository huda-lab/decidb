# N1 (Euclidean ball) — open work

N1 keeps one sum of squares inside a limit by scaling the direction or the target onto the ball. The class is in
[definition.md](definition.md); what N1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task.

## BLOAT-5 — Bloat in N1

- **Problem.** N1's rule (`n1_euclidean_ball_rule.cpp`, 647 lines) and its tests (`test_direct_n1_euclidean_ball.py`,
  1,002 lines, 172 cases, and its entry in `_direct_rule_fixtures.py`) have not been read for bloat.
- **Change.** Remove the bloat by `../../framework/todo.md` § How to work any task (Removing bloat), in these files
  only; a duplicate found in shared code is listed for BLOAT-1.
- **Done when.** The matrix passes with no answer, error text or golden changed, and the report lists each removal
  with its evidence and the files' lines, case count and `lizard` figures before and after.
