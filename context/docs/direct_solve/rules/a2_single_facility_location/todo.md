# A2 (single-facility location) — open work

A2 picks one shared value for many rows: one `per ():` or `per K:` decision placed at the best center of the rows'
observations. The class is in [definition.md](definition.md). How to work and the checks are in
`../../framework/todo.md` § How to work any task; a new rule follows `../../architecture/rules.md` § Adding a rule.

## A2-1 — Query-wide squared loss

- **Problem.** `DECIDE per (): c(REAL) SUCH THAT c BETWEEN lo AND hi MINIMIZE SUM(POWER(c - v, 2))` takes the solver
  path: the IR reads the `GLOBAL` decision, and every registered rule declines a decision that is not one per row.
- **Change.** Add rule A2 (`a2_single_facility_location_rule.cpp`). It applies to one `REAL` query-wide decision,
  constant bounds, and a `MINIMIZE` objective of one square of `c` minus a source value under a constant positive
  coefficient, with an optional `when`. The plan aggregates `AVG(v)` over the rows the objective keeps, clamps it to the
  interval, and repeats it on every source row. The aggregate-then-repeat fragment stays in the rule's files until a
  second rule needs it. Test what the plan returns when the objective keeps no row (every value in the interval is
  optimal) and how NULL, NaN or infinite values raise, as the solver does.
- **Ruling (owner, 2026-11-06).** When the objective keeps no row, the answer is the point of the interval nearest 0:
  `COALESCE(AVG(v), 0)` goes through the same clamp. It adds no branch and stays finite when the lower end is open,
  and it equals A1's lowest value in the box whenever the lower bound is 0 or above.
- **Done when.** The three standard test sets of `rules.md` exist for A2: a conformance fixture, a differential oracle
  file `test_direct_a2_single_facility_location.py`, and C++ decline cases for each `CheckCondition` branch. The worked
  example returns 5.0 under `require`. The other rules decline every query A2 takes.

## A2-2 — Keyed decision `per K:`

- **Problem.** `DECIDE per k: c(REAL) ... MINIMIZE SUM(POWER(c - v, 2))` gives one center per class of `k`, and A2-1
  takes only a query-wide decision.
- **Change.** Aggregate per key and join the center back to the rows of its class. A row whose key is NULL, and a
  class whose rows the objective's `when` drops, behave as on the solver path.
- **Needs.** A2-1.
- **Done when.** Oracle cases cover several keys, a NULL key, a multi-column key, and a class with no kept row. Each is
  answered under `require` and agrees with `off`.

## A2-3 — Bounds read from data on a shared decision

- **Problem.** Each row's `c <= cap` binds the same decision, so the solver intersects them. A2-1 takes only constant
  bounds.
- **Change.** Read the interval as `MAX(lower)` to `MIN(upper)` over the rows each bound clause keeps, per key for a
  keyed decision. An empty interval raises the solver's infeasible error. A NULL, NaN or infinite bound raises as the
  solver does.
- **Needs.** A2-2.
- **Done when.** Oracle cases cover a data upper bound, a data lower bound, both under a `when`, and an empty interval.
  The error table covers NULL and NaN bounds.

## A2-4 — Data weights on the square

- **Decision needed.** What A2 does with a negative weight.
- **Problem.** `SUM(w * POWER(c - v, 2))` with `w` read from the data takes the solver path. `DirectReadSquareTerm`
  declines a coefficient read from data. The objective is `W*c² - 2*c*SUM(w*v) + constant`, where `W` is the total
  weight. The plan is chosen before any weight is read, so a negative weight seen at run time cannot fall back to the
  solver; Gurobi answers such a query.
- **Options.**
  1. Take every sign. `W > 0`: the clamped weighted mean. `W < 0`: the better of the two bounds, or unbounded when
     that bound is infinite. `W = 0`: the bound the slope's sign picks, any value when the slope is 0. Cost: three
     branches. The definition's eligibility line on nonnegative weights is corrected.
  2. Decline data weights. They stay on the solver path.
- **Recommendation.** 1. The proof is one variable's quadratic, and A2 then answers where HiGHS refuses a data weight
  on a square.
- **Change.** Let the shared square reader read a coefficient from data, with the solver's NULL, NaN and infinity
  errors. A1 keeps declining it. Build the chosen option per instance (query-wide or per key). The framework's
  Number-range warning covers a total weight that cancels toward 0.
- **Needs.** A2-3.
- **Done when.** Oracle cases cover each branch the chosen option keeps, on both backends where the solver path
  answers.

## A2-5 — Absolute loss

- **Problem.** `SUM(ABS(c - v))` and `norm(c - v, 1)` take the solver path. The IR has no reading of an absolute-value
  term.
- **Change.** Read an absolute term of a shared decision minus a source value, with a constant or data weight. The
  answer is the weighted median: an ordered window over `v` with a running total of the weights, clamped to the
  interval. Any point between two middle values is optimal.
- **Needs.** A2-4.
- **Done when.** Oracle cases cover an odd count, an even count, ties, weights, a key, and a median outside the
  interval.

## A2-6 — Maximum absolute loss

- **Problem.** `MINIMIZE MAX(ABS(c - v))` and `norm(c - v, 'inf')` take the solver path.
- **Change.** Answer the midpoint of `MIN(v)` and `MAX(v)` over the kept rows, clamped, per instance.
- **Needs.** A2-5.
- **Done when.** Oracle cases cover the query-wide and keyed forms and a midpoint outside the interval.

## A2-7 — L0 loss

- **Decision needed.** Whether A2 takes `norm(c - v, 0)`.
- **Problem.** The L0 count is the number of rows where `|c - v|` reaches `decide_l0_tolerance`. The best `c` covers
  the most values within the tolerance. Matching the solver also means its rules for the inferred `M` and for a value
  forced strictly between 0 and the tolerance.
- **Options.**
  1. Decline it. L0 stays on the solver path, and A2's `done.md` and `definition.md` say so.
  2. Build it, with an oracle grid over the tolerance and `M`.
- **Recommendation.** 1. The proof is long for a rare loss, which `rules.md` § Keep the layer lean says to decline.
- **Change.** The chosen option.
- **Needs.** A2-6.
- **Done when.** Option 1: a near-miss case declines L0 under `require`. Option 2: oracle cases agree on both backends.

## A2-8 — Benchmark

- **Problem.** A2 has no measurement against Gurobi and HiGHS.
- **Change.** Add `a2.sql` (query-wide) and `a2_keyed.sql` to `benchmark/decide/direct_solve/queries/`. Run
  `run.py --queries a2 a2_keyed` at the small sizes, up to `sf=1`, and record the results in A2's `done.md`
  § Performance.
- **Needs.** A2-6.
- **Done when.** Every size agrees with the solvers, and the table is in `done.md`.
