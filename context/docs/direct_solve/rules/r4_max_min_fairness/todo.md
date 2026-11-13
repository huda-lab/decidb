# R4 (weighted max-min fair allocation) — open work

R4 shares one resource so that the smallest served fraction of demand is as large as possible. The class is in
[definition.md](definition.md); what R4 does today is in [done.md](done.md). How to work and the checks are in
`../../framework/todo.md` § How to work any task; a new rule follows `../../architecture/rules.md` § Adding a rule.

Every task reads the canonical form: `MAXIMIZE MIN(amount/demand)` is `MIN(c*x)` with the row coefficient
`c = 1/demand`, so other spellings of the same objective need no list. Every row takes the end of its range that uses
the least budget, by the sign of its weight; a row in the `MIN` has the range `[s/c, cap]` at level `s`, any other row
`[0, cap]`.

## R4-5 — Benchmark

- **Problem.** R4's cost against a hand plan and against Gurobi is unmeasured.
- **Change.** Add `r4` and `r4_groups` to `benchmark/decide/direct_solve/queries/` and `hand/` (weight from a column).
  Measure, against the hand plan and in a Gurobi sweep as N1's: the cost of the sign handling, R4's plan against a
  hand plan that assumes positive weights (inside noise, or D2 returns to the owner); and the planning time of a DECIDE
  query R4 declines, direct solve on against off (inside noise).
- **Done when.** `benchmark/decide/direct_solve/run.py --queries r4 r4_groups` runs at the default sizes, and the
  figures are in R4's `done.md`.
