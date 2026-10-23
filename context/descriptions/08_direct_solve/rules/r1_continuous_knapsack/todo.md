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

## R1-30 — Bring two decisions within twice one decision

- **Problem.** `done.md` § Performance: `two_decisions` at 5,000,000 rows runs 2.52 s against 0.60 s for one decision
  (4.2 times; 21.2 s of CPU against 4.9 s), where the target was 2 times. Every one of the four reads computes both
  decisions' item values, though stages 2 to 4 need only some of them, and the sort holds 10,000,000 items.
- **Change.** Profile `two_decisions` with `benchmark/decide/profile_r1_plan.py` (the profiler is honest now) and cut
  the work each read does to the columns its stage reads. Answers and errors stay the same.
- **Done when.** The five checks pass; `two_decisions` at 5,000,000 rows runs within 2 times `le_positive`.
