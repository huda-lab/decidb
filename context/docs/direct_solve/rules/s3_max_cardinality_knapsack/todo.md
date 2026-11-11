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

## S3-2 — S3's plan

- **Problem.** A recognized query has no plan, and the rule is not registered.
- **Change.** The rule is registered in `direct_registry.cpp`, and `Apply` builds the prefix plan:
  - the rows sorted by weight and a stable identity, with `DirectRunningSum` per budget group;
  - the empty prefix counted, and the largest prefix whose total is at most the group's budget selected;
  - no prefix that fits, the empty one included, raises the solver's infeasible error; an empty source gives an empty
    result;
  - every row is read; a NaN or infinite weight or budget raises in the solver's words through the shared value checks,
    and a NULL weight does what the solver path does with it (S3-3's oracle case says which);
  - constant weight terms whose sum passes a DOUBLE (`SUM((1e308 + 1e308)*x)`, admitted by the recognition) raise as
    the solver path does.
- **Done when.** The worked example in `definition.md` answers under `require` as under `off`, and
  `./test/decide/run_tests.sh -n 2 -k s3_max_cardinality` passes once S3-3 is combined.

## S3-3 — S3's standard tests

- **Problem.** S3 has none of the standard tests. They are written against the scope rulings; their `require` runs
  pass once S3-2 is combined.
- **Change.**
  - A `RuleFixture` in `_direct_rule_fixtures.py`, with near misses: a weight of exactly 1 (S1's), a per-row value in
    the objective, a strict `<` (over an integer weight: the binder refuses a strict `<` over a DOUBLE one), a `when`,
    a second constraint.
  - `test_direct_s3_max_cardinality_knapsack.py` with an oracle case table: positive, zero and negative weights, a
    negative budget met by negative weights, tied weights, a budget that fits every row and one that fits none (empty
    prefix), a data-valued budget, `by` groups with different budgets, a constant factor, duplicate rows, a NULL weight.
  - An error table (NaN and infinite weights, an infeasible negative budget) and a boundary table (a prefix total within
    rounding of `B`, against the solver only), and a parent-query case.
  - A `test_direct_user_facing.py` case comparing the infeasible text of `require` with `off`.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k s3_max_cardinality` passes (the fixture ids carry the rule name):
  every case agrees with the oracle and with `off`, and each near miss declines under `require`. Before S3-2 is
  combined, only the `off` runs pass.

## S3-4 — S3's docs

- **Needs.** S3-3.
- **Problem.** S3 has no `done.md`, and the direct-solve README lists it as not started.
- **Change.**
  - `done.md` in the done format: what it admits, what it declines, how it works, error wording, verified by.
  - `definition.md` corrected where the code proves something different (NULL weights, rounding at the budget).
  - S3's status row in `../../README.md` reads Works, the class count and summary include it, and its links table
    points to S3's `done.md` and `todo.md`.
- **Done when.** Every claim in `done.md` names the test that holds it. Doc-only: no tests.

## S3-5 — S3's Performance is measured

- **Needs.** S3-4. A quiet machine; the owner starts the run.
- **Problem.** `done.md` has no Performance section, which every working class has.
- **Change.** A hand plan `benchmark/decide/direct_solve/hand/s3.sql` and an S3 query in the benchmark, measured as
  `../../../performance/benchmarking/direct_solve.md` § Comparing with a hand-written plan describes, against Gurobi
  and HiGHS.
- **Done when.** `done.md` § Performance gives CPU against the hand plan and speed against both solvers.

## S3-6 — `when` on the budget and the objective

- **Needs.** S3-4. Optional.
- **Problem.** A `when` in front of the budget, inside its sum or on the objective declines, and users write it
  naturally.
- **Change.** Admit a deterministic source-only `when` in those places, read through `DirectOnKeptRows` as S1 and R1
  read theirs: a dropped row is outside the budget, reads no weight, and is not selected. An objective `when` keeps
  every kept row's coefficient equal, or the rule declines.
- **Done when.** Oracle cases for each `when` place, a NaN weight on a dropped row that does not raise, and
  `./test/decide/run_tests.sh -n 2 -k s3_max_cardinality` passes.

## S3-7 — Fixed items

- **Needs.** S3-4. Optional.
- **Problem.** A per-row bound that fixes an item (`when must: x = 1`, `x <= 0` on some rows) declines.
- **Change.** Read each per-row bound as the set of {0, 1} it allows, as S1 does; a row fixed to 1 is selected and its
  weight comes off its group's budget; a row fixed to 0 leaves the prefix; a row allowing neither is infeasible. The
  free rows take the longest prefix of the remaining budget.
- **Done when.** Oracle cases with fixed rows of each sign of weight, a fixed set over the budget (infeasible), and
  `./test/decide/run_tests.sh -n 2 -k s3_max_cardinality` passes.

## S3-8 — The fewest items that reach a minimum

- **Needs.** S3-4. Optional.
- **Problem.** `SUM(w*x) >= B MINIMIZE SUM(x)` (the fewest items whose weight reaches `B`) declines. The same exchange
  argument holds with the heaviest items first, but `definition.md` does not state the variant.
- **Change.** `definition.md` gains the variant and its proof; the rule admits it with the sort reversed and the
  shortest prefix whose total reaches `B`.
- **Done when.** Oracle cases for the variant (negative weights, no prefix reaching `B`), and
  `./test/decide/run_tests.sh -n 2 -k s3_max_cardinality` passes.
