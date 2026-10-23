# R1 — Continuous (fractional) knapsack — open work

R1 splits one shared budget among `REAL` decisions by filling the best value per unit of budget first. The class is in
[definition.md](definition.md); what R1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task. The goal is a plan near the floor that `done.md`
§ Performance measures, with the answers and errors R1 gives today.

## R1-24 — Read a source several times

- **Problem.** The plan of `definition.md` § The plan reads its source for four stages. No direct-solve fragment builds
  a second read of an arbitrary source.
- **Change.** A shared fragment beside `DirectExposeStoredRowId` (`direct_late_materialization.cpp`) that returns one
  read of the source per call. A source that function accepts (non-volatile projections and filters over one
  repeatable DuckDB table scan) is rebuilt with fresh table indexes, following the second scan that late
  materialization builds. Any other source is one `LogicalMaterializedCTE`, read through `LogicalCTERef`s. Nothing uses
  it until R1-25.
- **Done when.** `make release` passes and the five checks pass unchanged. Its branches are reached by R1-25's tests,
  which land with it if this task cannot be tested alone; say which.

## R1-25 — Build the plan for one decision with constant limits

- **Needs.** R1-24.
- **Problem.** `done.md` § Performance: one budget runs at Gurobi's speed, about 15 times the floor, on one core.
- **Change.** A new builder for the stages of `definition.md` § The plan, for one decision and constant limits, with
  and without a scope. It names its columns with one struct per stage holding that stage's `ColumnBinding`s. Tied
  items share in proportion to their room (§ Relational construction, step 6); a NULL or NaN value raises before a
  verdict. The rule routes these conditions to the new builder and every other condition to `PlanBuilder` until
  R1-27 (the strangler pattern: the two builders live side by side, and each later task moves more conditions over).
  The plan-shape tests change for the routed shapes: one sort over narrow columns, no window over source columns, one
  hash aggregate per group-level stage.
- **Done when.** The five checks pass; `coverage_report.py compare` lists no arm lost except arms of deleted code;
  `profile_r1_floor.py --rows 1000000` shows `le_positive` and `per_groups` within 1.5 times the floor and gaining from
  threads (CPU over wall 3 or more).

## R1-26 — Move several decisions to the new plan

- **Needs.** R1-25.
- **Problem.** Two decisions run at 0.3 times Gurobi's speed: the row number, the unnest of every source column and the
  hash group-back.
- **Change.** The new builder takes several decisions: stage 3 unnests only the narrow per-decision lists; stages 2 and
  4 sum over the decisions of a row; stage 5 answers each decision in its own column. The rule routes them to it.
  `PlanBuilder`'s row number, unnest and group-back are deleted with the tests that only they reached.
- **Done when.** The five checks pass; coverage compare as R1-25; `two_decisions` at 1,000,000 rows runs within 2 times
  `le_positive`.

## R1-27 — Move limits read from the data to the new plan

- **Needs.** R1-26.
- **Problem.** A limit read from the data is reduced per group by `DirectValidateBounds`, a window over every row.
- **Change.** Stage 2 takes the group's MIN and MAX of each data limit and checks every value, as § Admitted class
  states (an equality limit must agree within its group; NULL and NaN raise). The rule routes every condition to the
  new builder, and `PlanBuilder` is deleted with the tests that only it reached. `DirectValidateBounds` stays if S1 or
  another rule still uses it.
- **Done when.** The five checks pass; coverage compare as R1-25; no R1 plan holds a window over source columns.

## R1-18 — Re-run the performance sweep

- **Needs.** R1-27.
- **Problem.** `done.md` § Performance describes the plan before the redesign.
- **Change.** Run the sweep of `done.md` § Performance with the same commands, the 1,000,000-row sweep first; the
  5,000,000-row sweep takes about 20 minutes of full CPU. Replace the tables, the stamps and the "Where the time goes"
  text, and state which shapes beat Gurobi and which do not, with the reason. The target at 5,000,000 rows, all threads:
  `le_positive` within 1.5 times the floor, `per_groups` no slower than `le_positive`, `two_decisions` within 2 times
  `le_positive`, CPU over wall 3 or more, and peak memory below Gurobi's.
- **Done when.** `done.md` § Performance matches the measurements, every shape's objective agrees with Gurobi within
  1e-6 before timing, and the five checks pass.
