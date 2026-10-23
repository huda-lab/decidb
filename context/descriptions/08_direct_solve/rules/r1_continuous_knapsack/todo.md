# R1 — Continuous (fractional) knapsack — open work

R1 splits one shared budget among `REAL` decisions by filling the best value per unit of budget first. The class is in
[definition.md](definition.md); what R1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task. The goal is a plan that beats the solver on every shape
R1 admits, or a measured account of why a shape cannot.

## R1-11 — Choose how to make the plan cheaper than the solver on one global budget

- **Decision needed.**
- **Needs.** R1-13, R1-19 and R1-14.
- **Problem.** `done.md` § Performance: with one budget over the whole input, direct solve runs at 0.7x to 1.2x
  Gurobi's time and the solver's memory. Only `PER` shapes win. The plan's time does not fall with more threads on one
  global budget, and the floor's does.
- **Options.**
  1. Accept the numbers. R1 stays on by default because it is never wrong and beats HiGHS on every shape but two
     decisions. No work.
  2. Aggregate the groups (the aggregate-then-join pattern). Find each group's price and price sums over the narrow
     columns (keys, `t`, rooms), reduce them to one row per group, and hash-join them back to the full rows on the
     group keys. Items tied at the price share the remainder in proportion to their room, which needs no order, so
     the full rows never enter a window. About one day.
  3. Search for the price without a sort (bucket selection, the linear-time weighted-median idea). One hash aggregate
     sums the rooms per bucket of `t`; the bucket where the running total crosses the limit is the only one that
     needs a second pass. Parallel and linear. It needs its own proof in `definition.md`. About two days.
  4. Option 2 or 3, and R1 off by default for shapes it does not win, decided from the plan shape (no `PER`). About
     half a day more; it adds a cost rule to a layer that has none.
- **Recommendation.** Options 1 and 4 no longer fit: the hand-written narrow SQL beats Gurobi at every size
  (`done.md` § Performance), and it contains a sort. Choose between 2 and 3 after R1-19. Option 2 when the ladder shows
  that row width and the unpartitioned windows are the cost. Option 3 only when no plan with a sort comes near the floor.
- **Done when.** The chosen option is written into this task as its **Change**, then built; `done.md` § Performance
  (R1-18) shows the effect; the five checks pass. For option 3 also: the proof is in `definition.md`; R1 entries for
  the price-search terms are in the mutation harness (`../../framework/h35/run_mutations.py`) and every surviving
  mutation has a test that kills it; and a seeded random comparison of the search against the oracle and the solver
  path passes. Option 3 rewrites the verdict and price terms, which the hand-picked cases pin only in part.


## R1-13 — Profile the R1 plan operator by operator

- **Problem.** `done.md` § Performance says half the time is the sorted window and that every window carries every
  source column. Nothing records time, memory and threads per operator, how long planning takes, whether any operator
  spills, or how time grows with the width of the source.
- **Change.** Profile the plan with `EXPLAIN ANALYZE` (or the profiler's JSON output) at 1,000,000 and 5,000,000 rows,
  for `le_positive`, `per_groups`, `when_budget`, `count_only` and `two_decisions`. Record per operator the time, the
  rows, the peak memory and the threads in use. Record the time of planning and optimizing alone, and whether any
  operator writes to the temporary directory. Run `le_positive` on three copies of the source: the table as it is,
  one narrowed to the columns the query reads, and one widened by twenty more `DOUBLE` columns. Count the sorts,
  windows and materializations the plan holds, and list the columns each window outputs.
- **Done when.** `done.md` § Performance, "Where the time goes", is replaced by these numbers, with the time as a
  function of width, and each cost is named as made by the plan or by the method.

## R1-19 — Attribute the cost to plan features

- **Needs.** R1-13.
- **Problem.** A per-operator profile says where time is spent, not what removing a feature would save, because the
  operators of a pipeline overlap and compete for memory and cores.
- **Change.** A ladder of queries from the floor up to R1's plan, each adding one feature to the last, each timed as
  `benchmark/decide/profile_r1_floor.py` does: (1) the narrow floor; (2) the same on all source columns; (3) the four-window chain (totals, running sums,
  price, price sums) on narrow columns; (4) the chain on all columns; (5) the chain with each window that has no
  `ORDER BY` replaced by an ungrouped aggregate joined in; (6) the chain with a synthetic `PARTITION BY` of 2, 10 and
  1,000 keys; (7) the validation breaker, which holds every row until all are checked; (8) the per-row expression
  projections; (9) the real plan. For several decisions, a second ladder from the one-decision plan: row numbering, the
  unnest, and the hash group-back, one rung each. A rung that costs more than its parts is an interaction; report
  it.
- **Done when.** `done.md` § Performance holds the ladder (time, CPU over wall and memory per rung) and a ranked list of
  causes, each named as made by the plan or by the method with the rung that shows it. R1-14, R1-15 and R1-11 are
  rewritten where the ladder contradicts what they assume.


## R1-14 — Carry only the narrow columns through the windows

- **Needs.** R1-19.
- **Problem.** `done.md` says late materialization does not apply to R1's plan, so every window and the sort carry
  every source column and peak memory is the solver's size. S1 and A1 use late materialization
  (`direct_late_materialization.cpp`).
- **Change.** The windows and the sort read only the columns they use (group keys, `t`, rooms, a row identifier); the
  answer reaches the source rows by joining on the row identifier. Make the existing late materialization apply, or
  give the rule the same effect, and say in `architecture/` which standard pattern it follows. Answers and errors
  stay identical, and the plan keeps one sort.
- **Done when.** The five checks pass; `test_direct_r1_continuous_knapsack.py` still asserts the plan holds one sort;
  a regression asserts that no window of the plan outputs a source column; R1-13's profile, re-run, shows the change.

## R1-15 — Make several decisions no slower than one

- **Needs.** R1-19.
- **Problem.** Two decisions run at 0.3x Gurobi's speed. The plan numbers the rows, unnests each into one item per
  decision carrying every source column, and groups the items back with a hash aggregate that applies `any_value` to
  each source column.
- **Change.** Remove the cost R1-19 attributes to the unnest and the group-back. The target is a plan for two
  decisions no slower than the one-decision plan on the same number of items. Answers and errors stay identical.
- **Done when.** The five checks pass; the two-decision tests of `test_direct_r1_continuous_knapsack.py` still pass;
  R1-13's profile, re-run on the two-decision shape, shows the change.


## R1-17 — Choose how `PlanBuilder` names its columns

- **Decision needed.**
- **Needs.** R1-11, R1-14 and R1-15.
- **Problem.** `PlanBuilder` keeps its columns in `std::map<string, Column>` and reads them as `columns.at("c" + suffix)`.
  A wrong name is an exception at run time, not an error at compile time. The class is about 840 lines.
- **Options.**
  1. Named constants for the literal names (`constexpr const char *W = "w"`) and a helper for the suffixed ones. A typo
     becomes a compile error. About two hours.
  2. One struct per stage whose fields are the stage's `ColumnBinding`s (the stage's output schema). The compiler
     checks every use, and each stage states what it reads and writes. About half a day; it changes every stage.
  3. Leave it.
- **Recommendation.** Option 1. Option 2 when R1-14 splits the plan into a narrow part and a wide part, because the
  schemas then differ per part.
- **Done when.** The five checks pass and the plan the rule builds is unchanged (the plan-shape tests pass).

## R1-18 — Re-run the performance sweep

- **Needs.** R1-11 (its chosen option built), R1-14 and R1-15.
- **Problem.** `done.md` § Performance describes the plan before the performance work.
- **Change.** Run the sweep of `done.md` § Performance with the same command. Run the 1,000,000-row sweep first; the
  5,000,000-row sweep takes about 20 minutes of full CPU. Replace the table, the stamp and the "Where the time goes"
  text, and state which shapes beat Gurobi and which do not, with the reason.
- **Done when.** `done.md` § Performance matches the measurements, every shape's objective agrees with Gurobi within
  1e-6 before timing, and the five checks pass.
