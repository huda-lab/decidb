# S3 (maximum-cardinality knapsack) — open work

S3 selects as many `BOOL` items as fit one budget by sorting them by weight and taking the longest affordable prefix.
The class is in `definition.md`. How to work is in `../../framework/todo.md` § How to work any task. A task runs only
the focused tests its **Done when** names; the full gate runs once at combine. The standard tests are in
`../../architecture/rules.md` § The standard tests every rule ships.

## Scope rulings (owner, 2026-11-11)

The first version admits only the shape below and declines everything else. A decline is a slower correct answer; a
wrong admit is a wrong answer.

- **Decision.** One row-scoped `BOOL` decision.
- **Constraint.** Exactly one budget `SUM(w*x) <= B`. `w` is a numeric source column or deterministic expression of
  any sign, zero included. `B` is a constant, a source column or a reducer over the data, read by the shared
  `DirectReadLimit` as R1 reads its own. A `by` key gives each group its own budget. A body whose weight is exactly 1
  or -1 is a count and stays S1's. No strict `<`, no `=`, no `when`, no other constraint.
- **Objective.** `MAXIMIZE SUM(x)` under a positive constant factor (`2 * SUM(x)`, `SUM(3*x)`), with an optional
  constant term. No per-row coefficient (that is the general 0-1 knapsack), no `when`.
- **Plan.** Sort by weight with a stable identity, `DirectRunningSum` per group, the empty prefix included, the largest
  prefix whose total fits, `x = 1` on it. No prefix that fits is the solver's infeasible error.
- **Lean.** No new shared helper until a second rule needs it; no case outside the shapes above.

## S3-5 — S3's Performance is measured

- **Needs.** A quiet machine; the owner starts the run.
- **Problem.** `done.md` has no Performance section, which every working class has.
- **Change.** A hand plan `benchmark/decide/direct_solve/hand/s3.sql` and an S3 query in the benchmark, measured as
  `../../../performance/benchmarking/direct_solve.md` § Comparing with a hand-written plan describes, against Gurobi
  and HiGHS.
- **Done when.** `done.md` § Performance gives CPU against the hand plan and speed against both solvers.

## S3-6 — `when` on the budget and the objective

- **Optional.**
- **Problem.** A `when` in front of the budget, inside its sum or on the objective declines, and users write it
  naturally.
- **Change.** Admit a deterministic source-only `when` in front of the budget or inside its sum, read through
  `DirectOnKeptRows` as S1 and R1 read theirs: a dropped row reads no weight and is in no budget, and it still counts
  in the objective, so it is selected, as a row with a NULL `by` key is. An objective `when` is admitted only when it
  keeps exactly the budget's rows (the same condition): a row dropped from both adds nothing and costs nothing, and is
  not selected. Any other objective `when` gives the rows of one budget values of 0 and 1, a two-value knapsack, and
  declines.
- **Ruling (owner, 2026-11-11).** The semantics above; the fixture's near miss "a `when` on the budget" becomes an
  applies form, and an objective `when` that differs from the budget's takes its place as the near miss.
- **Done when.** Oracle cases for each `when` place, a NaN weight on a dropped row that does not raise, and
  `./test/decide/run_tests.sh -n 2 -k s3_max_cardinality` passes.

## S3-7 — Fixed items

- **Optional.**
- **Problem.** A per-row bound that fixes an item (`when must: x = 1`, `x <= 0` on some rows) declines.
- **Change.** Read each per-row bound as the set of {0, 1} it allows, as S1 does; a row fixed to 1 is selected and its
  weight counts toward its group's budget; a row fixed to 0 leaves the prefix; a row allowing neither is infeasible,
  and a constant bound that no `BOOL` meets declines as in S1 (`DirectContradictsDomain`). The free rows take the
  longest prefix whose total, added to the fixed rows' total, is at most `B`.
- **Ruling (owner, 2026-11-11).** The comparison is `fixed total + prefix total <= B` in DOUBLE, exact, as in the
  plan without fixed rows.
- **Done when.** Oracle cases with fixed rows of each sign of weight, a fixed set over the budget (infeasible), and
  `./test/decide/run_tests.sh -n 2 -k s3_max_cardinality` passes.

## S3-8 — The fewest items that reach a minimum

- **Optional.**
- **Problem.** `SUM(w*x) >= B MINIMIZE SUM(x)` (the fewest items whose weight reaches `B`) declines.
- **Change.** The rule admits `SUM(w*x) >= B` with a minimized positive count (`MINIMIZE SUM(x)`, or `MAXIMIZE`
  its negation), with the sort reversed (heaviest first) and the shortest prefix whose total reaches `B`: the MIN of
  the reaching ranks, the empty prefix included (`0 >= B` selects nothing). No prefix that reaches `B` is the solver's
  infeasible error. A data limit's group value is its MAX. The variant and its proof are in `definition.md`
  § The fewest items that reach a minimum.
- **Ruling (owner, 2026-11-11).** Only the two pairings: `<=` with a maximized count, `>=` with a minimized one. The
  other two are trivial (select nothing, or everything) and decline.
- **Done when.** Oracle cases for the variant (negative weights, no prefix reaching `B`), and
  `./test/decide/run_tests.sh -n 2 -k s3_max_cardinality` passes.
