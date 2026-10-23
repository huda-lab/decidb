# R1 — Continuous (fractional) knapsack — open work

R1 splits one shared budget among `REAL` decisions by filling the best value per unit of budget first. The class is in
[definition.md](definition.md). How to work and the checks are in `../../framework/todo.md` § How to work any task. The
goal is the full definition with its § Admitted class.

## R1-11 — Make the plan cheaper than the solver on one global budget

- **Decision needed.**
- **Problem.** `done.md` § Performance: with one budget over the whole input, direct solve runs at 0.7x to 1.2x
  Gurobi's time and the solver's memory (3 to 5 GiB at 5,000,000 rows); two decisions are 0.3x. Only `PER` shapes win
  (2.6x to 2.8x). The sorted window is half the time, every window carries every source column, and a window with no
  `PARTITION BY` is one partition.
- **Options.**
  1. Accept the numbers: R1 stays on by default because it is never wrong and beats HiGHS on every shape but two
     decisions. No work.
  2. Narrow the price search: project only the group keys, `t` and the rooms, find each group's price and price sums
     over that narrow input (sort, windows, then an aggregate to one row per group), and broadcast them back to the
     full rows with a hash join on the group keys (the standard aggregate-then-join pattern). Items tied at the price
     share the remainder in proportion to their room, which needs no order, so the full rows never enter a window. Two
     decisions unnest only the narrow columns. About one day, plus the same tests.
  3. Option 2 and also turn R1 off by default for shapes it does not win, decided from the plan shape (no `PER`).
     About half a day more; it adds a cost rule to a layer that has none.
- **Recommendation.** Option 2. It addresses the cause (wide rows in whole-input windows) for every shape, keeps one
  sort, and keeps R1 on by default without a cost rule.
- **Done when.** `done.md` § Performance, re-measured with the same command, shows the change against the table there;
  the five checks pass.
