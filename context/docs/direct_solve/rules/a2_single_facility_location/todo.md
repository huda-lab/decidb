# A2 (single-facility location) — open work

A2 picks one shared value for many rows: one `per ():` or `per K:` decision placed at the best center of the rows'
observations. The class is in [definition.md](definition.md). How to work and the checks are in
`../../framework/todo.md` § How to work any task; a new rule follows `../../architecture/rules.md` § Adding a rule.

## A2-8 — Benchmark

- **Problem.** A2 has no measurement against Gurobi and HiGHS.
- **Change.** Add `a2.sql` (query-wide) and `a2_keyed.sql` to `benchmark/decide/direct_solve/queries/`. Run
  `run.py --queries a2 a2_keyed` at the small sizes, up to `sf=1`, and record the results in A2's `done.md`
  § Performance.
- **Done when.** Every size agrees with the solvers, and the table is in `done.md`.

## A2-9 — Data weights on the absolute loss

- **Decision needed.** Whether A2 takes `SUM(w * ABS(c - v))` with `w` read from the data.
- **Problem.** The weighted median is optimal only when every weight is nonnegative. A negative weight makes the loss
  non-convex, the plan is chosen before any weight is read, and the solver path answers such a query (Big-M).
- **Options.**
  1. Keep declining it. Cost: none; weighted medians over data weights stay on the solver path.
  2. Evaluate the loss at every breakpoint inside the interval and at both ends, from running sums of `m` and `m*v`
     per class, and take the smallest (unbounded when the total weight is negative and an end is infinite). It covers
     every sign with no branch, at the cost of two more windows and values that can cancel.
- **Recommendation.** 1, until a query needs it.
- **Done when.** Option 2: oracle cases with mixed-sign weights agree on both backends.
