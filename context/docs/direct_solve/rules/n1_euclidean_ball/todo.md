# N1 (Euclidean ball) — open work

N1 keeps one sum of squares inside a limit by scaling the direction or the target onto the ball. The class is in
[definition.md](definition.md); what N1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task.

- **The owner's choice: scores and targets at extreme scales.** N1 overshoots the ball below about 1e-154 and raises
  too-large past about 1e154, where the true answer exists (`done.md` § Where it differs from the solver). Dividing by the
  group's largest value before squaring would give the true answer at every scale and remove the check of `h` past a
  DOUBLE, at the cost of a second window on every plan. Its cost is not measured.
- **Sizes past `sf=1`.** `benchmark/decide/direct_solve/run.py --queries n1 n1_groups` has run up to `sf=1`. The larger
  sizes are the owner's to start.
