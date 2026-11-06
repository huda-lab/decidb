# Query Diagnostics — Unbounded — open work

Open work in the unbounded diagnosis. Current behavior is in [done.md](done.md). How to work and the checks are in
`../../direct_solve/framework/todo.md` § How to work any task.

## UNB-1 — Name the runaway decision of a model with a quadratic row

- **Problem.** An unbounded model with a quadratic objective or constraint diagnoses as one `undiagnosed` finding,
  `a non-linear term prevents naming the variable.` (`decide_diagnostic.cpp`), even when the runaway column appears only
  in linear terms. Repro, with Gurobi: `DIAGNOSE` of `SUM(POWER(x, 2)) by (k) <= 25 MAXIMIZE SUM(s * x)` over rows
  `(k, s)` with one `k` NULL and `s` not 0: the NULL-key row is in no ball, so its `x` has no limit.
- **Change.** Extract the ray where the escaping columns have no quadratic term, so the finding names the decision and
  the row; keep the `undiagnosed` finding where a quadratic term holds the runaway.
- **Done when.** The repro names `x` on the NULL-key row and matches an independent solver's unbounded direction; a
  model whose runaway sits in a square stays `undiagnosed`.
