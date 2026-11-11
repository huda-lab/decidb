# S3 (maximum-cardinality knapsack) — what works today

Verified on 2026-11-11 at `509918dd97`: `test/decide/run_direct_matrix.sh`, each of its four suites (auto, off,
serializer, highs) 6,786 passed; C++ `build/release/test/unittest "[decidb]"` 6,879 assertions in 106 cases. Code:
`src/optimizer/decide/direct/s3_max_cardinality_knapsack_rule.cpp`. Class definition: [definition.md](definition.md).

## What it admits

As many rows as fit one budget:

```sql
SELECT id, selected
FROM items
DECIDE selected(BOOL)
SUCH THAT SUM(weight*selected) <= 9
MAXIMIZE SUM(selected);
```

- **The decision.** One row-scoped `BOOL`, with no keyed scope.
- **The budget.** Exactly one constraint, `SUM(w*x) <= B`, a plain `SUM` of linear terms in `x`, and no other
  constraint of any kind. A `by` key gives each group its own budget; a row with a NULL in a key element that does not
  group NULLs (D5) is in no budget.
- **The weight.** `w` is the signed sum of the terms' coefficients: finite constants and numeric values of the source
  row (`w`, `-w`, `w + cap`, a cast that may raise), of any sign, zero included. Constant coefficients that add up to
  exactly 1 or -1 make the body a count, which is S1's, so S3 declines it and the two rules stay disjoint.
- **The limit.** `B` is read by the shared `DirectReadLimit`, as R1 reads its own: a constant of magnitude below 1e20,
  a numeric value of the source row, or a reducer of the group (`<= 2 * COUNT(*) by (g)`). A value that varies in a
  group is limited by its group's MIN. A NULL limit skips its instance, so a group whose limits are all NULL has no
  budget.
- **The objective.** `MAXIMIZE` a positive constant times the count: `SUM(x)`, `2 * SUM(x)`, `SUM(3*x)`, and
  parts and terms whose constant coefficients net to a positive count, with an optional constant term. Every selected
  row adds the same amount, which is what makes the cheapest rows the best ones.

## How it works

The `k` smallest weights have the smallest total of any `k` rows, so a group fits `k` rows exactly when its `k`
lightest rows fit, and the answer is the longest prefix of the rows sorted by weight whose total is within `B`. The plan
reads the source once and uses windows only, no hash aggregate joined back:

1. The scope projection (`DirectProjectScope`), then each row's weight (`DirectSumOf` of the budget's terms) and what
   the data limit reads.
2. One window, per group, in order of the weight: the running total (`DirectRunningSum`), the row's rank
   (`ROW_NUMBER`) and the group's limit (`DirectWindowSink`, `DirectAddGroupBound`). The rank and the running total
   share one partition and one order, so DuckDB sorts once (`plan_window.cpp`, `PartitionsAreEquivalent`) and a row's
   rank is the position its total was summed at. Two sorts could order tied rows differently, and then select tied
   rows whose total exceeds `B`.
3. The checks: a NaN or infinite weight (`DirectCheckNumber`), the limit's checks, and a running total past a DOUBLE
   (`DirectRaiseTooLarge`, naming the budget). Then each row's rank when its prefix fits.
4. A second window: `k`, the group's largest rank that fits. Weights are sorted ascending, so with negative weights the
   totals fall before they rise; the plan takes the largest fitting rank, not the first that fails. The empty prefix
   (total 0) counts.
5. Where a group can fit nothing (a data limit, or a constant limit below 0), a `bool_and` window over all rows checks
   that every group fits a prefix and raises `DirectInfeasibleErrorPredicate` before the first row leaves, even under
   `LIMIT 1`.
6. The answer: the source columns, `x = 1` when the row's rank is at most `k` (or the row is in no budget), and the
   column of the checks (`validated_columns`).

A NULL weight reads 0, as the solver path's front end guards the term. An empty source gives an empty result,
whatever the limit. `EXPLAIN` shows `S3_MAX_CARDINALITY_KNAPSACK` and `max c*SUM(x), SUM(w*x) <= B`.

## What it does not admit (falls back to the solver)

A query S3 does not apply to keeps its answer: it runs on the solver, and `require` raises one fixed error. Open work
on these shapes is in [todo.md](todo.md).

| Shape | Why |
|---|---|
| A weight of exactly 1 or -1 (`SUM(x) <= 5`, `SUM(0.5*x + 0.5*x)`) | A count, which S1 answers. |
| A value per row in the objective (`MAXIMIZE SUM(v*x)`) | The general 0-1 knapsack: the cheapest rows need not be the best. |
| `>=`, `=` or a strict `<` on the budget; a scaled budget (`2 * SUM(w*x) <= 18`) | Not one upper budget in the plain form. |
| A `when` on the budget, in its sum or on the objective; a `per K:` class | A smaller scope the plan does not model. |
| A second constraint of any kind, a per-row bound that fixes an item | Not one budget alone. |
| A constant limit of magnitude 1e20 or more, a weight or limit fixed only within the query (`txid_current()`) | The solver's reading of such a constant depends on the rows; a value that is not a value of the row is not read. |
| An objective coefficient read from the data, a constant term inside the `SUM`, a minimized count, no objective | Not a positive count to maximize. |
| `INT` or `REAL` decisions, two decisions, a keyed decision | Not one `BOOL` per row. |

## Where it differs from the solver

- **Ties.** Rows of equal weight make several answers optimal; S3 may select different rows than the solver.
- **The budget is compared exactly.** A prefix total is compared with `B` as DuckDB computes it in DOUBLE, as S1 reads
  its bounds. The solver accepts a violation of about 1e-6: weights 0.1 and 0.2 under `<= 0.3` (whose DOUBLE sum is
  0.30000000000000004) select both rows on Gurobi and HiGHS, and one row on direct, whose answer satisfies
  `SUM(w*x) <= 0.3` as DuckDB evaluates it. To avoid the difference, round the weights.
- **A running total past a DOUBLE raises.** Weights -1e308, -1e308, 1e308, 1e308, 1e308 under `<= 0` reach a DOUBLE
  total of -inf, which no later weight raises again, so the plan raises the solver's TOO_LARGE text naming the budget
  rather than answer, as R1 raises when its own plan computes a number past a DOUBLE. The solver path warns and
  answers (there, all five rows, over the budget).

**Error wording** (`../../architecture/policy.md`). The messages are the solver's, built by the same functions: a NaN
or infinite weight names the values it is made of (`w is NaN or infinite on some rows`), a NaN limit raises the solver's
text for it, the infeasible error is `DecideInfeasibleMessage`,
and constant weights that add up past a DOUBLE raise the solver's TOO_LARGE text.

## Tests

- Conformance: the `S3` fixture in `test/decide/tests/_direct_rule_fixtures.py` (the worked example; near misses for a
  value per row in the objective, a strict `<` over an integer weight, a `when` on the budget and a second constraint;
  `2 * SUM(x)` and a constant weight of 2 apply; a NaN weight on the last of 5,000 rows).
- Differential oracle: `test/decide/tests/test_direct_s3_max_cardinality_knapsack.py`, `CASES`: the worked example in
  both orders, zero and negative weights, a negative budget met by negative weights, ties, a budget that fits every row
  and one that fits only the empty prefix, a column limit and a group reducer limit, `by` groups with column and
  constant budgets, rows whose `by` key is NULL, constant factors outside and inside the sum, a constant weight,
  duplicate rows and a NULL weight; a joined source, a parent `COUNT(*)`, `LIMIT` and filter; an empty source.
- S1's counts: `test_s3_leaves_a_count_to_s1` (`EXPLAIN` names S1 for four spellings of a count).
- Errors on both paths: `test_s3_error_cases_fail_on_both_paths` (NaN, +inf and -inf weights, a NaN limit, a negative
  constant budget no prefix meets, one infeasible group), and
  `test_s3_constant_weights_that_add_up_past_a_double_raise_the_solvers_error`.
- Differences: `test_s3_direct_compares_a_prefix_total_with_the_budget_exactly_in_a_double`,
  `test_s3_a_running_total_past_a_double_raises_where_the_solver_answers`.
- Ties under a parallel sort: `test_s3_a_budget_that_ends_inside_a_run_of_ties_holds_when_the_sort_runs_in_parallel`
  (250,000 rows over more than one row group, four threads; below about 200,000 rows DuckDB keeps one tie order).
- Error text: `test_s3_infeasible_error_is_the_solvers_error_word_for_word` in `test_direct_user_facing.py`.
- C++: `S3AppliesShapes` and `S3DeclineShapes` in `test/common/test_decidb_direct_decline.cpp`, one decline per
  `CheckCondition` branch, and the partition test against every other rule.
