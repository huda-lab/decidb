# A2 (single-facility location) — open work

A2 picks one shared value for many rows: one `per ():` or `per K:` decision placed at the best center of the rows'
observations. The class is in [definition.md](definition.md). How to work and the checks are in
`../../framework/todo.md` § How to work any task; a new rule follows `../../architecture/rules.md` § Adding a rule.

## A2-4 — Data weights on the square

- **Problem.** `SUM(w * POWER(c - v, 2))` with `w` read from the data takes the solver path. `DirectReadSquareTerm`
  declines a coefficient read from data. The objective is `W*c² - 2*c*SUM(w*v) + constant`, where `W` is the total
  weight. The plan is chosen before any weight is read, so a negative weight seen at run time cannot fall back to the
  solver; Gurobi answers such a query.
- **Ruling (owner-delegated, 2026-11-07).** Take every sign. `W > 0`: the clamped weighted mean. `W <= 0`: an end of
  the interval, the upper one when `a*(L+U) + b < 0` (as `f(U) - f(L) = (U-L)*(a*(U+L) + b)`, with no square of a
  large end); an infinite chosen end raises the solver's unbounded error. When several points are optimal, the one
  nearest 0 (the A2 tie rule). The definition's eligibility line on nonnegative weights is corrected.
- **Change.** One representation of a square's weight, no flag: `DirectSquareTerm` carries its weight as signed values
  (`vector<DirectSignedValue>` and their names) in place of a constant `scale`; `DirectLayoutQuadraticPart` lays out a
  per-row `k` column with the solver's NULL, NaN and infinity checks (none for a constant weight); `DirectFoldQuadratic`
  reads that column. N1 keeps declining a data weight through `DirectFoldConstantSum`; A1 states its own class with one
  line that declines a weight read from the data. The IR front end guards a square's coefficient against NULL as it
  guards the other terms (`GuardCoefficients`), so a NULL weight skips its row as on the solver path. The framework's
  Number-range warning covers a total weight that cancels toward 0.
- **Done when.** Oracle cases cover `W > 0`, `W < 0` with each end chosen, `W = 0` with each slope sign and a zero
  slope, an unbounded end, a NULL and a NaN weight, keyed and query-wide, on both backends where the solver path answers.

## A2-5 — Absolute loss

- **Problem.** `SUM(ABS(c - v))` and `norm(c - v, 1)` take the solver path. The IR reads the absolute term (an opaque
  term with its inner terms), and no shared reader reads it.
- **Ruling (owner-delegated, 2026-11-07).** A constant weight, positive once the sense is applied; a weight read from
  the data is declined (A2-9). The answer among the optimal points is the one nearest 0, the A2 tie rule:
  `clamp(clamp(0, lower median, upper median), L, U)`.
- **Change.** Extract the inner-form loop of `DirectReadSquareTerm` into a shared `DirectReadLinearForm` and add
  `DirectReadAbsTerm` over it, kept out of `DirectReadQuadraticPart` so A1 and N1 need no new decline. In A2, one loss
  Strategy per loss behind the plan's stage skeleton (Template Method): each loss gives its per-row columns, its window
  aggregates, any extra stage, and its target; the shared tail (nearest 0, clamp, no-class NULL, empty-interval raise)
  stays in one place. The median: a running sum `R` of the weights per class in `v` order beside the total `M`; the
  lower median is `MIN(v)` where `2R >= M`, the upper `MAX(v)` where `2(M - R + m) >= M`, on one sort. R1's
  `RunningSum` moves to the shared fragments as the second user takes it.
- **Done when.** Oracle cases cover an odd count, an even count, ties, a constant weight, a key, a median outside the
  interval, and the nearest-0 choice inside a flat optimum.

## A2-6 — Maximum absolute loss

- **Problem.** `MINIMIZE MAX(ABS(c - v))` and `norm(c - v, 'inf')` take the solver path.
- **Ruling (owner-delegated, 2026-11-07).** A constant positive weight under `MINIMIZE` only; any other weight or sense
  is declined.
- **Change.** A third loss Strategy: the midpoint of `MIN(v)` and `MAX(v)` over the kept rows, as `lo/2 + hi/2`, clamped,
  per instance; no kept row gives no target, and the shared tail answers the point nearest 0.
- **Done when.** Oracle cases cover the query-wide and keyed forms, a midpoint outside the interval, and values near
  the DOUBLE limit.

## A2-7 — L0 loss

- **Problem.** `norm(c - v, 0)` takes the solver path.
- **Ruling (owner-delegated, 2026-11-07).** Decline it. The proof is long for a rare loss (`rules.md` § Keep the layer
  lean). The IR already marks it not proved.
- **Change.** A2's `done.md` and `definition.md` say L0 stays on the solver path.
- **Done when.** A near-miss case declines L0 under `require`.

## A2-8 — Benchmark

- **Problem.** A2 has no measurement against Gurobi and HiGHS.
- **Change.** Add `a2.sql` (query-wide) and `a2_keyed.sql` to `benchmark/decide/direct_solve/queries/`. Run
  `run.py --queries a2 a2_keyed` at the small sizes, up to `sf=1`, and record the results in A2's `done.md`
  § Performance.
- **Needs.** A2-6.
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
