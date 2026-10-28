# R1 — Continuous (fractional) knapsack — open work

R1 splits one shared budget among `REAL` decisions by filling the best value per unit of budget first. The class is in
[definition.md](definition.md); what R1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task. The goal is a plan near the floor that `done.md`
§ Performance measures, with the answers and errors R1 gives today.

## Read a `by` key again (one budget per group)

The language groups a budget on the reducer (`sum(w*x) by (g) <= B`, or one clause per class with
`per g: sum(w*x) by (g) <= B`), and R1 declines both. Each group should get its own budget and its own price. The per-group plan is kept (`group_key_slots`, the SINGLE join on the group key)
and reached only through IR edits (`PerKeyOnConstraint` in `test/common/test_decidb_direct_decline.cpp`).

The per-group plan kept in the code does the following, which is the spec:

- Each group a budget and a price; clauses merged into one budget only with the same keys in the same order.
- A data-valued limit reduced per group (upper: MIN, lower: MAX, equality: one value per group). A limit on a sum with
  its own `when` was reduced over the budget's rows and the other rows with the same keys, by a window over the group
  rows partitioned by the keys alone.
- A NULL key, like a row the `when` drops, left the row out of every budget; it took its own best end.
- A group whose rows all failed the `when` had no budget; a `when` that covered no row raised the empty-aggregate
  error, from a window that counted the eligible groups.
- The per-group rows of the Performance table in `done.md` measure this plan (16x over Gurobi at 1,000 groups).

Where the language differs from that plan:

- A NULL key is in no group, so it has no budget (D5); the row takes its own best end.
- A group the sum's own `when` leaves empty has no budget (D68); its rows take their own best ends.
- `per g:` in front of a clause does not group its sum (D6); only a `by` key does.
- Nothing is computed on a row a `when` drops (syntax reference §6.3).

Needs: `../../framework/todo.md` § H-04 and § H-05.

Tests: remove the `xfail(strict=True)` marks of the plan-shape tests and delete the `by` branch of `direct_path` so the `by` cases
in `test_direct_r1_continuous_knapsack.py` run on both paths. `test_r1_scoped_plan_groups_with_hash_aggregates_and_joins_on_the_key`
still counts the deleted guard window; re-derive its counts. The leave-to-solver grid's
`by` cases (`test_r1_leaves_budgets_that_do_not_share_a_scope_to_the_solver`) decline on the key today, not on the
scope mismatch they were written for. The direct-solve benchmark's `r1.sql`
(`benchmark/decide/direct_solve/queries/`) fails in `direct` mode; run it again, and once S1 reads it too, drop the
sentence about it in `../../../performance/benchmarking/direct_solve.md`.
