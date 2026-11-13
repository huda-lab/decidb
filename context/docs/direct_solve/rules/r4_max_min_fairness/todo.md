# R4 (weighted max-min fair allocation) — open work

R4 shares one resource so that the smallest served fraction of demand is as large as possible. The class is in
[definition.md](definition.md). How to work and the checks are in `../../framework/todo.md` § How to work any task; a
new rule follows `../../architecture/rules.md` § Adding a rule.

Every task reads the canonical form: `MAXIMIZE MIN(amount/demand)` is `MIN(c*x)` with the row coefficient
`c = 1/demand`, so other spellings of the same objective need no list. Every row takes the end of its range that uses
the least budget, by the sign of its weight; a row in the `MIN` has the range `[s/c, cap]` at level `s`, any other row
`[0, cap]`.

## R4-1 — Plain rule: constant positive weight, one budget

- **Problem.** The worked example's form with a constant weight takes the solver path; no rule reads a maximized `MIN`
  of a linear term.
- **Change.** Add rule R4 (`r4_max_min_fairness_rule.cpp`). It applies to one `REAL` decision per row with lower bound
  0 and an upper bound that is a constant, a column or absent; a `MAXIMIZE` objective of one `MIN` over one term `c*x`
  with a row coefficient and no constant; and one `SUM(w*x) <= B` with `w` and `B` constants and `w > 0`. One
  aggregate gives the level `s = min(B / SUM(w/c), MIN(cap*c))`; each row's amount is `s/c`, joined back as N1 joins
  its group values. As the solver answers (ruling D3, 2026-11-13): a demand of 0 raises the solver's NaN or infinite
  message for `1 / demand`; any `c <= 0` in the `MIN` gives level 0 and every amount 0; a row the `MIN` does not read
  (NULL demand, an objective `when`) gets 0. A NULL capacity is no upper bound; a negative capacity or budget raises
  the solver's infeasible error; an empty source returns no rows. Settle on the solver path, and test, what a `MIN`
  that reads no row returns.
- **Done when.** The three standard test sets of `rules.md` exist for R4: a `RuleFixture` in
  `_direct_rule_fixtures.py`, the oracle file `test_direct_r4_max_min_fairness.py` (its check reads the objective value
  and feasibility, since the amounts are not unique), C++ decline cases for each `CheckCondition` branch and R4's row in
  `EveryRuleWithItsShapes`. The worked example with weight 1 returns 3 and 6 under `require`. Every other rule declines
  R4's shapes. `./test/decide/run_tests.sh -n 2 -k "r4 or conformance"` and `build/release/test/unittest "[decidb]"`
  pass.

## R4-2 — Weights of any sign

- **Problem.** The worked example reads its weight from a column, whose sign the plan cannot see; R4-1 declines it.
- **Change.** Take a weight read from data or a constant of any sign (ruling D2, 2026-11-13). A weight of 0 adds
  nothing to the sum. A row with a negative weight takes its capacity, and its freed budget `-w*cap` enters the same
  aggregate; such a row with no capacity makes the budget unlimited, and the capacities decide the level. The budget
  is infeasible when what is left after those rows is negative, so a negative `B` can be feasible. NULL, NaN or
  infinite weights raise as the solver does. Correct the eligibility lines of `definition.md` that ask for strictly
  positive coefficients.
- **Needs.** R4-1.
- **Done when.** Oracle cases cover a column weight with mixed signs, a zero weight, a negative weight with and
  without a capacity, a negative budget that the freed budget makes feasible, and the worked example from a column;
  the error table covers NULL and NaN weights. `./test/decide/run_tests.sh -n 2 -k "r4 or conformance"` and
  `build/release/test/unittest "[decidb]"` pass.

## R4-3 — Budget per group and a `when` on the budget

- **Problem.** A `by` budget, its limit a constant or read from data, and a budget under a `when` take the solver path.
- **Change.** Each budget group gets its own level `s_g` from its own rows (ruling D1, 2026-11-13). A group with no
  finite level (no binding budget, no capacity) and any row outside every budget take the overall level
  `s* = min_g s_g`; when no group has a finite level the query raises the solver's unbounded error. Read the limit as
  N1 does (`DirectReadLimit`): a constant, a column constant within its group, or a NULL key group as the solver
  treats it.
- **Needs.** R4-2.
- **Done when.** Oracle cases cover two groups at different levels, a group limited by capacity, a group with no
  finite level, a row outside every budget, a data limit, a NULL key, and the unbounded case; the conformance fixture
  gains about 50 groups of `late_errors`. `./test/decide/run_tests.sh -n 2 -k "r4 or conformance"` and
  `build/release/test/unittest "[decidb]"` pass.

## R4-4 — Decision shared by key (`per K:`)

- **Problem.** `DECIDE per k: amount(REAL)` gives one amount to every row of a key; R4-3 takes only row decisions.
- **Change.** Per key, add the weights, keep the smallest capacity and the smallest coefficient `c` of the rows the
  `MIN` reads (any `c <= 0` gives level 0); the rest is the per-row construction on one row per key, repeated on every
  row of its key. A NULL key behaves as on the solver path.
- **Needs.** R4-3.
- **Done when.** Oracle cases cover several keys, a key with mixed weights, a key with one row outside the `MIN`, a
  NULL key, and a key under a `by` budget. `./test/decide/run_tests.sh -n 2 -k "r4 or conformance"` and
  `build/release/test/unittest "[decidb]"` pass.

## R4-5 — Benchmark

- **Problem.** R4's cost against a hand plan and against Gurobi is unmeasured.
- **Change.** Add `r4` and `r4_groups` to `benchmark/decide/direct_solve/queries/` and `hand/` (weight from a column).
  Measure, against the hand plan and in a Gurobi sweep as N1's: the cost of the sign handling, R4's plan against a
  hand plan that assumes positive weights (inside noise, or D2 returns to the owner); and the planning time of a DECIDE
  query R4 declines, direct solve on against off (inside noise).
- **Needs.** R4-4.
- **Done when.** `benchmark/decide/direct_solve/run.py --queries r4 r4_groups` runs at the default sizes, and the
  figures are in R4's `done.md`.
