# R1 — Continuous (fractional) knapsack — open work

R1 splits one shared budget among `REAL` decisions by filling the best value per unit of budget first. The class is in
[definition.md](definition.md); what R1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task. The goal is a plan near the floor that `done.md`
§ Performance measures, with the answers and errors R1 gives today.

## R1-20 — Design the plan to the floor's shape

- **Decision needed.** The owner approves the design before R1-21 builds it.
- **Problem.** `done.md` § Performance: the floor, a hand-written query of the same method, runs about 15 times faster
  than the direct plan at 5,000,000 rows and beats Gurobi at every size, so the time is the plan's. The plan differs
  from the floor in four ways:
  1. One pipeline computes every case the class admits (negative and zero weights, infinite rooms, `>=` and `=`
     limits, ties, groups) on every query, whatever the query holds.
  2. Every stage forwards every column, so the windows carry 24 to 36 columns where the floor's sort carries 3.
  3. Whole-input totals (the anchors' spend, the rooms, the extreme prices) are windows that hold every row, where an
     ungrouped aggregate gives one row and runs in parallel.
  4. Finding the price takes three windows after the sort (candidates, sums at the price, ties); the floor reads each
     row's answer off its running sum.
  The plan keeps about one core busy and gains nothing from more threads; the floor runs 3 times faster with them.
- **Change.** Write the new plan into `definition.md` § Relational construction (the only copy of the construction),
  stage by stage. For each stage: the operator, the columns it carries, whether it holds every row, and the standard
  pattern it follows (`.claude/CLAUDE.md` § Design). Also state:
  - how each case of § Admitted class maps onto the plan, and which work a query skips because its proved condition
    rules a case out (decided at plan time from the condition, never from the data);
  - how the errors keep today's behavior: every row checked before any is released, infeasible before unbounded;
  - how several decisions run (today: row number, unnest, hash group-back; `done.md` shows two decisions at 0.3 times
    Gurobi's speed). The target is two decisions no slower than one decision on the same number of items;
  - how the source columns reach the output without riding through the sort (late materialization, as S1 and A1 do in
    `direct_late_materialization.cpp`, or a join back on a row identifier);
  - how `PlanBuilder` names its columns. Today it keeps them in `std::map<string, Column>`, so a wrong name fails at
    run time. Options: named constants (a typo becomes a compile error), or one struct per stage holding that stage's
    `ColumnBinding`s (the compiler checks every use);
  - the cost target against the floor, for `le_positive`, `per_groups` and `two_decisions` at 5,000,000 rows.
  Inputs: the floor query in `benchmark/decide/profile_r1_floor.py`; the plan today in
  `src/optimizer/decide/direct/r1_continuous_knapsack_rule.cpp` (`PlanBuilder`); the profile tool
  `benchmark/decide/profile_r1_plan.py`. Two known ways to find the price: per group by aggregate-then-join (each
  group's price and price sums over the narrow columns, reduced to one row per group, joined back on the group keys;
  items tied at the price share the remainder in proportion to their room, which needs no order), or without a sort
  by bucket selection (the linear-time weighted-median idea; it needs its own proof). Change § Correctness wherever
  the construction it argues about changes.
- **Recommendation.** The floor's shape: one sort over the narrow columns with a running sum; group totals by an
  aggregate joined back; each row's answer from its running sum and its group's price in one projection; the source
  columns joined back after the sort; and constants for column names. Keep the sort and leave bucket selection out
  unless the design cannot come near the floor with it.
- **Done when.** The owner approves the design; `definition.md` § Relational construction and § Correctness describe
  it; R1-21 lists its build steps, split into tasks of one sitting each where it is larger.

## R1-21 — Build the redesigned plan

- **Needs.** R1-20.
- **Problem.** The plan R1-20 replaces runs at Gurobi's speed on one global budget, at 0.3 times it with two decisions,
  and with the solver's memory (`done.md` § Performance).
- **Change.** Build the plan `definition.md` § Relational construction describes, in
  `src/optimizer/decide/direct/r1_continuous_knapsack_rule.cpp` and the shared fragments it uses. Answers and errors
  stay what they are today. The plan-shape tests (one sort, one unnest, one aggregate) change only where the design
  says the shape changes. Code the redesign deletes takes with it the tests that only reached that code
  (`.claude/CLAUDE.md` § Testing Guidelines).
- **Done when.** The five checks pass; `coverage_report.py compare` lists no arm lost except arms of deleted code;
  `profile_r1_floor.py` meets R1-20's cost target at 5,000,000 rows and shows the plan gaining from threads; peak memory
  is below Gurobi's. If the design finds the price without a sort, also: R1 entries for the price terms are in the
  mutation harness (`../../framework/h35/run_mutations.py`) and every surviving mutation has a test that kills it, and
  a seeded random comparison of the plan against the oracle and the solver path passes.

## R1-18 — Re-run the performance sweep

- **Needs.** R1-21.
- **Problem.** `done.md` § Performance describes the plan before the redesign.
- **Change.** Run the sweep of `done.md` § Performance with the same commands, the 1,000,000-row sweep first; the
  5,000,000-row sweep takes about 20 minutes of full CPU. Replace the tables, the stamps and the "Where the time goes"
  text, and state which shapes beat Gurobi and which do not, with the reason.
- **Done when.** `done.md` § Performance matches the measurements, every shape's objective agrees with Gurobi within
  1e-6 before timing, and the five checks pass.
