# Query Diagnostics — Unbounded — open work

Open work in the unbounded diagnosis. Current behavior is in [done.md](done.md). How to work and the checks are in
`../../direct_solve/framework/todo.md` § How to work any task.

## UNB-1 — Name the runaway decision of a model with a non-linear part

- **Ruling (owner, 2026-11-06).** One rule for every non-linear part: a square or product (objective or row), a
  general constraint a backend states natively (ABS, MIN, MAX) and an indicator constraint. Each column in one of them
  stays still along the ray.
- **Problem.** An unbounded model with a quadratic objective or constraint diagnoses as one `undiagnosed` finding,
  `a non-linear term prevents naming the variable.` (`decide_diagnostic.cpp`), even when the runaway column appears only
  in linear terms. Repro, with Gurobi: `DIAGNOSE` of `SUM(POWER(x, 2)) by (k) <= 25 MAXIMIZE SUM(s * x)` over rows
  `(k, s)` with one `k` NULL and `s` not 0: the NULL-key row is in no ball, so its `x` has no limit.
- **Change.** `BuildUnboundedRayFallbackModel` (`probe_models.cpp`) builds its ray LP from the linear rows and from
  the linear part of each quadratic row, each with a right-hand side of 0. It fixes at 0 every column that appears in a
  non-linear part, instead of declining the model. Along such a ray every non-linear part keeps its value, so a
  feasible point moved along it stays feasible and the objective improves linearly. The ray is a real escape, a part of
  the recession cone, never a relaxation. A model with no non-linear part gets the same LP as today. A runaway inside a
  non-linear part finds no ray and keeps the `undiagnosed` finding and its reason.
- **Done when.**
  - The repro names `x` on the NULL-key row. A gurobipy oracle confirms it: the model is unbounded, and bounded once
    that row's `x` has a bound.
  - `MAXIMIZE SUM(POWER(x, 2))` over an open `x` stays `undiagnosed`.
  - One case each for a native MIN/MAX and for an indicator row names the linear runaway.
  - The linear-model ray tests pass unchanged.
