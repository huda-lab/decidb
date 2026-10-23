# R1 — Continuous (fractional) knapsack — open work

R1 splits one shared budget among `REAL` decisions by filling the best value per unit of budget first. The class is in
[definition.md](definition.md); what R1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task. The goal is a plan that beats the solver on every shape
R1 admits, or a measured account of why a shape cannot.

## R1-11 — Choose how to make the plan cheaper than the solver on one global budget

- **Decision needed.**
- **Needs.** R1-12, R1-13 and R1-14.
- **Problem.** `done.md` § Performance: with one budget over the whole input, direct solve runs at 0.7x to 1.2x
  Gurobi's time and the solver's memory. Only `PER` shapes win. A window with no `PARTITION BY` is one partition, so
  each whole-input window is one large unit of work.
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
- **Recommendation.** Choose after R1-12. Option 2 when the hand-written narrow SQL of R1-12 beats Gurobi's time.
  Option 3 only when a plan with a sort cannot. Option 1 or 4 when nothing beats Gurobi.
- **Done when.** The chosen option is written into this task as its **Change**, then built; `done.md` § Performance
  (R1-18) shows the effect; the five checks pass. For option 3 also: the proof is in `definition.md`; R1 entries for
  the price-search terms are in the mutation harness (`../../framework/h35/run_mutations.py`) and every surviving
  mutation has a test that kills it; and a seeded random comparison of the search against the oracle and the solver
  path passes. Option 3 rewrites the verdict and price terms, which the hand-picked cases pin only in part.

## R1-12 — Measure the least the plan could cost

- **Problem.** No one knows whether R1's time on one global budget is the cost of the method or of our plan. There is
  no floor to compare against. The sorted window takes 0.81 s of 1.52 s at 1,000,000 rows, which looks like far more
  than a sort of the same numbers.
- **Change.** A benchmark script under `benchmark/decide/` that runs on the same databases as
  `profile_direct_r1.py` and times:
  1. A hand-written DuckDB query of the narrow shape (keys, `t`, rooms; sort by `t`; running sum; the price; the
     answer for every row by a join back), at one thread and at all threads.
  2. A bare scan of the source that writes one `DOUBLE` per row, the cost of reading and returning the rows.
  3. The solver path split into model load, solve and result return, from the solver's own log or timers around
     those calls.
- **Done when.** `done.md` § Performance lists the floor beside the direct and Gurobi times for the `<=` shape at
  1,000,000 and 5,000,000 rows, and says which reading holds: R1 is at the floor, R1 is above the floor because of the
  plan, or the floor is above Gurobi's time. The script is committed.

## R1-13 — Profile the R1 plan operator by operator

- **Problem.** `done.md` § Performance says half the time is the sorted window and that every window carries every
  source column. Nothing records time, memory and threads per operator, or whether a window with no `PARTITION BY`
  uses more than one thread.
- **Change.** Profile the plan with `EXPLAIN ANALYZE` (or the profiler's JSON output) at 1,000,000 and 5,000,000
  rows, for the `<=` positive-weight shape and the two-decision shape. Record per operator the time, the rows, the
  peak memory and the threads in use (CPU time over wall time). Run the same query on a source table that holds only
  the columns the query reads, to isolate the cost of width. Count the sorts and materializations the plan holds.
- **Done when.** `done.md` § Performance, "Where the time goes", is replaced by these numbers, and each cost is named
  as made by the plan (width, one-thread windows, extra materialization) or by the method (the sort itself).

## R1-14 — Carry only the narrow columns through the windows

- **Needs.** R1-13.
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

- **Needs.** R1-13.
- **Problem.** Two decisions run at 0.3x Gurobi's speed. The plan numbers the rows, unnests each into one item per
  decision carrying every source column, and groups the items back with a hash aggregate that applies `any_value` to
  each source column.
- **Change.** Remove the cost R1-13 attributes to the unnest and the group-back. The target is a plan for two
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
