# R1 — Continuous (fractional) knapsack — open work

R1 splits one shared budget among `REAL` decisions by filling the best value per unit of budget first. The class is in
[definition.md](definition.md); what R1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task. The goal is a plan near the floor that `done.md`
§ Performance measures, with the answers and errors R1 gives today.

## R1-27 — Move limits read from the data to the new plan

- **Needs.** R1-26.
- **Problem.** A limit read from the data is reduced per group by `DirectValidateBounds`, a window over every row.
- **Change.** Stage 2 takes the group's MIN and MAX of each data limit and checks every value, as § Admitted class
  states (an equality limit must agree within its group; NULL and NaN raise). The rule routes every condition to the
  new builder, and `PlanBuilder` is deleted with the tests that only it reached. `DirectValidateBounds` stays if S1 or
  another rule still uses it.
- **Done when.** The five checks pass; `coverage_report.py compare` lists no arm lost except arms of deleted code; no R1 plan holds a window over source columns.

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
