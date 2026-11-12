# S3 (maximum-cardinality knapsack) — what works today

Verified on 2026-11-11 at `a94e16e743`: `test/decide/run_direct_matrix.sh`, each of its four suites (auto, off,
serializer, highs) 6,864 passed; C++ `build/release/test/unittest "[decidb]"` 7,233 assertions in 106 cases. Code:
`src/optimizer/decide/direct/s3_max_cardinality_knapsack_rule.cpp`. Class definition: [definition.md](definition.md).

## What it admits

As many rows as fit one budget, or the fewest rows that reach one minimum:

```sql
SELECT id, selected
FROM items
DECIDE selected(BOOL)
SUCH THAT SUM(weight*selected) <= 9
MAXIMIZE SUM(selected);
```

- **The decision.** One row-scoped `BOOL`, with no keyed scope.
- **The budget.** Exactly one aggregate constraint, `SUM(w*x) <= B` with a maximized count or `SUM(w*x) >= B` with a
  minimized one, a plain `SUM` of linear terms in `x`. A `by` key gives each group its own budget; a row with a NULL in
  a key element that does not group NULLs (D5) is in no budget. A deterministic `when` over source columns, in front
  of the budget or inside its sum, drops a row from the budget: it reads no weight, and a NaN weight there raises
  nothing.
- **Fixed items.** Any other constraint is a per-row clause on `x` (`when must: x = 1`, `x <= flag`, `x <> c`), read
  by the shared `DirectReadFixedValues` as S1 reads its own: the set of {0, 1} each row allows. A value or coefficient
  may be an expression of the row that may raise (`x >= CAST(w AS TINYINT) - 2`); it is read only on the rows the
  clause keeps, as the solver reads it. A row fixed to 1 is
  selected and its weight counts toward its group's budget; a row fixed to 0 is not selected; a row that allows
  neither is infeasible.
- **The weight.** `w` is the signed sum of the terms' coefficients: finite constants and numeric values of the source
  row (`w`, `-w`, `w + cap`, a cast that may raise), of any sign, zero included. Constant coefficients that add up to
  exactly 1 or -1 make the body a count, which is S1's, so S3 declines it and the two rules stay disjoint.
- **The limit.** `B` is read by the shared `DirectReadLimit`, as R1 reads its own: a constant of magnitude below 1e20,
  a numeric value of the source row, or a reducer of the group (`<= 2 * COUNT(*) by (g)`). A value that varies in a
  group is limited by its group's MIN. A NULL limit skips its instance, so a group whose limits are all NULL has no
  budget.
- **The objective.** A positive constant times the count, maximized under `<=` and minimized under `>=` (`SUM(x)`,
  `2 * SUM(x)`, `SUM(3*x)`, parts and terms whose constant coefficients net to a count, `MAXIMIZE` a negated count
  for a minimum), with an optional constant term. Every selected row adds the same amount, which is what makes the
  lightest rows the best ones under a budget and the heaviest under a minimum. An objective `when` is admitted only
  when it is the budget's own `when` in every part.
- **Rows outside every budget.** A row dropped by the budget's `when` or by a NULL key still counts in the objective:
  under `<=` it is selected, since it costs nothing; under `>=` it is not, since it reaches nothing. A row the
  objective's `when` drops too adds nothing and is not selected.

## How it works

The `k` smallest weights have the smallest total of any `k` rows, so a group fits `k` rows exactly when its `k`
lightest rows fit, and the answer is the longest prefix of the rows sorted by weight whose total is within `B`. Under
`>=` the `k` largest weights have the largest total, and the answer is the shortest prefix of the rows sorted heaviest
first whose total reaches `B` (`definition.md` § The fewest items that reach a minimum). Fixed rows add a constant to
every prefix total, so the free rows take the prefix whose total, added to the fixed rows' total, meets `B`. One plan
serves both directions; it reads the source once and uses windows only, no hash aggregate joined back. It is the
framework's ranked prefix (`DirectRankedPrefix`, [framework](../../framework/done.md)), with W2 for the prefix size:

1. The scope projection (`DirectProjectScope`), then each row's weight (`DirectSumOf` of the budget's terms, NULL on a
   row the budget's `when` drops or a fixed row) and what the data limit reads, on the rows the budget keeps.
2. One window, per group, in order of the weight (ascending under `<=`, descending under `>=`, a NULL weight last): the
   running total (`DirectRunningSum`), the row's rank (`ROW_NUMBER`), the total of the rows fixed to 1 and the group's
   limit (`DirectWindowSink`, `DirectAddGroupBound`; its MIN under `<=`, its MAX under `>=`). The rank and the running
   total share one partition and one order, so DuckDB sorts once (`plan_window.cpp`, `PartitionsAreEquivalent`) and a
   row's rank is the position its total was summed at. Two sorts could order tied rows differently, and then select tied
   rows whose total exceeds `B`.
3. The checks: a NaN or infinite weight (`DirectCheckNumber`), the fixed values' checks, the limit's checks, and a fixed
   total, a running total or their sum past a DOUBLE (`DirectRaiseTooLarge`, naming the budget). Then each row's rank
   when `fixed total + prefix total` meets `B`. A fixed row's NULL weight sorts last, so its total is the whole free
   prefix's and it meets `B` only when every free row does.
4. A second window: `k`, the group's largest rank that fits under `<=`, its smallest rank that reaches `B` under `>=`.
   With negative weights the totals fall before they rise (rise before they fall under `>=`), so the plan takes the
   extreme fitting rank, not the first that fails. The empty prefix (the fixed total alone) counts.
5. Where a group can meet nothing (a data limit, fixed rows, a constant limit below 0 under `<=` or above 0 under `>=`),
   the held rows (`DirectHeldRows`, a materialized stage over every row) check that every group meets `B` with some
   prefix and raise `DirectInfeasibleErrorPredicate` before the first row leaves, even under `LIMIT 1`.
6. The answer: the source columns, `x` (the fixed value on a fixed row; outside every budget, as above; else rank at
   most `k`), and the column of the checks (`validated_columns`).

A NULL weight reads 0, as the solver path's front end guards the term. An empty source gives an empty result,
whatever the limit. `EXPLAIN` shows `S3_MAX_CARDINALITY_KNAPSACK` and
`max c*SUM(x), SUM(w*x) <= B; min c*SUM(x), SUM(w*x) >= B`.

## What it does not admit (falls back to the solver)

A query S3 does not apply to keeps its answer: it runs on the solver, and `require` raises one fixed error. Open work
on these shapes is in [todo.md](todo.md).

| Shape | Why |
|---|---|
| A weight of exactly 1 or -1 (`SUM(x) <= 5`, `SUM(0.5*x + 0.5*x)`) | A count, which S1 answers. |
| A value per row in the objective (`MAXIMIZE SUM(v*x)`) | The general 0-1 knapsack: the cheapest rows need not be the best. |
| `=` or a strict `<` on the budget; a scaled budget (`2 * SUM(w*x) <= 18`) | Not one budget in the plain form. |
| `<=` with a minimized count, `>=` with a maximized one | Trivial (nothing, or every row) and not the class. |
| An objective `when` that is not the budget's; a `per K:` class | The rows of one budget would be worth 0 or 1, a two-value knapsack; a `per K:` class is a scope the plan does not model. |
| A second aggregate constraint, a per-row constraint on `x` that is not a fixed value, a `when` that reads more than the source | Not one budget with fixed items. |
| A constant limit of magnitude 1e20 or more, a weight or limit fixed only within the query (`txid_current()`) | The solver's reading of such a constant depends on the rows; a value that is not a value of the row is not read. |
| An objective coefficient read from the data, a constant term inside the `SUM`, no objective | Not a count. |
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
  answers (there, all five rows, over the budget). Fixed rows whose total, alone or with a prefix, passes a DOUBLE
  raise the same way.

**Error wording** (`../../architecture/policy.md`). The messages are the solver's, built by the same functions: a NaN
or infinite weight names the values it is made of (`w is NaN or infinite on some rows`), a NaN limit raises the solver's
text for it, the infeasible error is `DecideInfeasibleMessage`,
and constant weights that add up past a DOUBLE raise the solver's TOO_LARGE text.

## Tests

- Conformance: the `S3` fixture in `test/decide/tests/_direct_rule_fixtures.py` (the worked example; near misses for a
  value per row in the objective, a strict `<` over an integer weight, an objective `when` that is not the budget's, a
  second constraint and `>=` with a maximized count; `2 * SUM(x)`, a constant weight of 2, a budget `when`, the same
  `when` on the objective, a row fixed to 1 and `>=` with a minimized count apply; a NaN weight on the last of 5,000
  rows).
- Differential oracle: `test/decide/tests/test_direct_s3_max_cardinality_knapsack.py`, `CASES`: the worked example in
  both orders, zero and negative weights, a negative budget met by negative weights, ties, a budget that fits every row
  and one that fits only the empty prefix, a column limit and a group reducer limit, `by` groups with column and
  constant budgets, rows whose `by` key is NULL, constant factors outside and inside the sum, a constant weight,
  duplicate rows and a NULL weight; a `when` in front of the budget, inside its sum and both, with negative weights,
  `by` groups and limits read on kept rows, and the objective's `when` equal to the budget's; rows fixed to 1 and to 0
  of each sign of weight, constant and data bounds, a fixed total meeting the budget exactly or rescued by a negative
  free weight, fixed rows outside every budget, a bound and a coefficient that are expressions of the row, each also
  with its raising value on a row the clause's `when` drops; the `at_least_*` cases for `>=` (the worked example, totals
  that rise then fall, a minimum of 0, `by` groups, a data minimum's MAX, a NULL key, a `when`, fixed items); a joined
  source, with and without a `when`, a parent `COUNT(*)`, `LIMIT` and filter; an empty source. Each case is the one test
  that catches some mutation of the plan or its recognition.
- A dropped row: `test_s3_a_nan_weight_on_a_dropped_row_raises_on_neither_path`.
- S1's counts: `test_s3_leaves_a_count_to_s1` (`EXPLAIN` names S1 for four spellings of a count).
- Errors on both paths: `test_s3_error_cases_fail_on_both_paths` (NaN, +inf and -inf weights, a NaN limit, a negative
  constant budget no prefix meets, one infeasible group; a fixed set over the budget, a row allowing neither value, a
  constant bound no `BOOL` meets under a `when`, a NaN bound; under `>=`, no prefix reaching `B`, only negative
  weights, one unreachable group, a NaN weight and a NaN limit), and
  `test_s3_constant_weights_that_add_up_past_a_double_raise_the_solvers_error`, and
  `test_s3_a_fixed_value_that_raises_on_a_kept_row_raises_the_solvers_error` (a bound and a coefficient, word for
  word with the solver's error).
- Differences: `test_s3_direct_compares_a_prefix_total_with_the_budget_exactly_in_a_double`,
  `test_s3_a_running_total_past_a_double_raises_where_the_solver_answers`,
  `test_s3_fixed_rows_past_a_double_raise_where_the_solver_answers`.
- Ties under a parallel sort: `test_s3_a_budget_that_ends_inside_a_run_of_ties_holds_when_the_sort_runs_in_parallel`
  (250,000 rows over more than one row group, four threads; below about 200,000 rows DuckDB keeps one tie order).
- Error text: `test_s3_infeasible_error_is_the_solvers_error_word_for_word` in `test_direct_user_facing.py`.
- C++: `S3AppliesShapes` and `S3DeclineShapes` in `test/common/test_decidb_direct_decline.cpp`, one decline per
  `CheckCondition` branch, and the partition test against every other rule.

## Performance

Measured on 2026-11-12 at `ec72e2c6f5`, on an 11-core Apple M3 Pro at a load of 13 to 16 (other work ran beside it);
the `s3_global` rows against Gurobi on 2026-11-11 at `4988843298`, at a load of 1.9 to 4.2, over the same plan.

**Against a hand-written plan.** `benchmark/decide/direct_solve/run.py --compare --sf 1 --queries s3 s3_global` runs the
direct plan beside a query written by hand for the same answer (`hand/<query>.sql`), in one process, best of 5, over the
800,000 rows of `partsupp`. CPU is every operator's time summed; latency is on all threads; the planning DECIDE adds is
apart; "1 thread / all" is the latency on one thread over the latency on all. Both sides gave the same answer.

| Query | Direct CPU | Hand CPU | CPU ratio | Direct latency | Hand latency | DECIDE planning | 1 thread / all (direct, hand) |
|---|---:|---:|---:|---:|---:|---:|---:|
| `s3` | 341.1 ms | 222.9 ms | 1.53 | 56.6 ms | 37.4 ms | 0.11 ms | 3.09, 3.44 |
| `s3_global` | 290.4 ms | 279.2 ms | 1.04 | 179.6 ms | 93.0 ms | 0.10 ms | 1.09, 1.84 |

The hand plans are one running-sum window cut at `total <= B`, which TPC-H's positive costs make correct. The direct
plan also finds the largest rank that fits (step 4 of § How it works), which negative weights need, so `s3` takes 1.53
times the hand plan's CPU. `s3_global` takes the same CPU as its hand plan but twice its latency, and gains nothing
from threads: with no `by`, step 4's window has an empty `OVER ()`, which DuckDB's `PhysicalWindow` treats as
order-dependent and fills from one thread, so the sorted running-sum window before it also runs on one thread. DuckDB
does this to return rows in table order, which other queries rely on. A constant partition fills it from every thread
(80 to 105 ms) but sorts every row, at 1.8 times the hand plan's CPU, so the plan keeps the one-thread window
(`../../framework/todo.md` § How to work any task, Small gains).

**Against Gurobi.** `run.py --sf 0.125 0.25 0.5 1 --queries s3 s3_global --modes direct gurobi`: a fresh `decidb`
process per run, median of 3. At every size Gurobi's objective agreed with direct solve's within 1e-6. A speedup
divides Gurobi's median by direct's median from the same sweep. Times include process start and database open, about
20 ms.

| Query | Rows | Direct | Direct peak | Gurobi | Gurobi peak | Speedup |
|---|---:|---:|---:|---:|---:|---:|
| `s3` | 100,000 | 0.058 s | 63 MiB | 0.610 s | 101 MiB | 10.4x |
| `s3` | 200,000 | 0.063 s | 80 MiB | 1.299 s | 166 MiB | 20.5x |
| `s3` | 400,000 | 0.074 s | 105 MiB | 2.377 s | 297 MiB | 32.2x |
| `s3` | 800,000 | 0.094 s | 146 MiB | 4.770 s | 570 MiB | 50.5x |
| `s3_global` | 100,000 | 0.044 s | 31 MiB | 2.024 s | 198 MiB | 45.7x |
| `s3_global` | 200,000 | 0.072 s | 44 MiB | 6.469 s | 278 MiB | 89.9x |
| `s3_global` | 400,000 | 0.116 s | 65 MiB | 10.403 s | 409 MiB | 89.5x |
| `s3_global` | 800,000 | 0.207 s | 103 MiB | 22.978 s | 665 MiB | 110.8x |

The solver path builds one binary decision per row and a knapsack row per budget, and must prove that no other
selection fits more rows; many rows of equal cost make many selections equally good. Direct solve is ahead at every
size, 50 to 111 times Gurobi at 800,000 rows, in a fourth to a sixth of its peak memory.
