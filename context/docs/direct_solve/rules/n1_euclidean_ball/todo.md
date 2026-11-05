# N1 (Euclidean ball) — open work

N1 keeps one sum of squares inside a limit by scaling the direction or the target onto the ball. The class is in
[definition.md](definition.md); what N1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task.

- **The owner's choice: the plan's breakers.** A limit read from the data adds a window, and a `per K:` a NULL can leave
  with no instance adds another, so every check on a row stands below the last breaker. The check of `h` past a DOUBLE
  stands above it, so under `LIMIT` another group's overflow is not seen. `done.md` § How it works gives the reasons.
- **Sizes past `sf=1`.** `benchmark/decide/direct_solve/run.py --queries n1 n1_groups` has run up to `sf=1`. The larger
  sizes are the owner's to start.
- **Scores whose squares reach the subnormal range.** Below about 1e-154 the sum of squares loses precision, and the
  answer can overshoot the ball (`done.md` § Where it differs from the solver). Dividing by the group's largest value
  before squaring would hold it, at the cost of a second window. Worth it only if a user reaches that range.
