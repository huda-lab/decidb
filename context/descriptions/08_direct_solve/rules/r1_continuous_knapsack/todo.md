# R1 — Continuous (fractional) knapsack — open work

R1 splits one shared budget among `REAL` decisions by filling the best value per unit of budget first. The class is in
[definition.md](definition.md); what R1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task. The goal is a plan near the floor that `done.md`
§ Performance measures, with the answers and errors R1 gives today.

## R1-28 — Find the one-off garbage rows under the serializer check

- **Problem.** Once, during R1-27's first run of `DECIDB_VERIFY_SERIALIZER=1 make decide-test`,
  `test_cons_correlated_subquery.py::test_correlated_subquery_is_real` returned garbage rows (an `l_orderkey` near
  4.6e18, `x` near 1e-317). Its source is not a table scan, so R1 materializes it (`LogicalMaterializedCTE` in
  `DirectSourceReader`). It did not recur in 40 runs of the test alone, 10 parallel runs of it with every R1 test, and
  three full serializer suites.
- **Change.** Run the test in a loop under an AddressSanitizer build (and under the serializer check) until it fails
  or a few hundred runs pass; if it fails, name the cause with the stack. Check whether `LogicalCTERef` or
  `LogicalMaterializedCTE` keeps column types, names and the materialize flag through serialization.
- **Done when.** The cause is fixed with a regression that fails without the fix, or `done.md` § Where it differs
  records the sanitizer runs that found no fault.

## R1-18 — Re-run the performance sweep

- **Problem.** `done.md` § Performance describes the plan before the redesign.
- **Change.** Run the sweep of `done.md` § Performance with the same commands, the 1,000,000-row sweep first; the
  5,000,000-row sweep takes about 20 minutes of full CPU. Replace the tables, the stamps and the "Where the time goes"
  text, and state which shapes beat Gurobi and which do not, with the reason. The target at 5,000,000 rows, all threads:
  `le_positive` within 1.5 times the floor, `per_groups` no slower than `le_positive`, `two_decisions` within 2 times
  `le_positive`, CPU over wall 3 or more, and peak memory below Gurobi's.
- **Done when.** `done.md` § Performance matches the measurements, every shape's objective agrees with Gurobi within
  1e-6 before timing, and the five checks pass.
