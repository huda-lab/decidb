# R1 (continuous knapsack) — what works today

Verified on 2026-10-23 on top of `d808c5f9f2` with the trimmed R1 tests: `make decide-test`,
`DECIDB_TEST_DIRECT_SOLVE=off make decide-test` and `DECIDB_VERIFY_SERIALIZER=1 make decide-test` each 2,987 passed (no
failures, no expected failures); `DECIDB_FORCE_SOLVER=highs make decide-test` 2,986 passed and one failure that needs Gurobi
(`test_norm_combined_l1_l2_objective`); `build/release/test/unittest "[decidb]"` 2,302 assertions in 59 cases.
`test/decide/run_direct_matrix.sh` renews these numbers in one command (it prints one line per check).
Code: `src/optimizer/decide/direct/r1_continuous_knapsack_rule.cpp`; its per-row bounds and the linear terms of the
budget and the objective come from the shared reader in `direct_bound_reader.cpp`. Class definition and correctness
argument: [definition.md](definition.md).

## What it admits

One or more row-scoped `REAL` decisions with:

- **Bounds, per row.** Every per-row bound the shared reader reads for A1: constants, columns and expressions, with a
  coefficient, an offset and a `WHEN` on the clause. Lower bounds may be positive or negative. A bound of magnitude
  1e20 or more is no bound, as on the solver, so a row may be uncapped, floorless or free.
- **One budget, `L <= SUM(w*x) <= U`.** A plain `SUM` of linear terms in `x`. The weight `w` is a column or
  expression of any sign, read per row, and zero on some rows is allowed. The budget is written `<=`, `>=`, `=` or
  `BETWEEN`, or as the same sum compared several times: aggregate constraints with the same terms in the same order
  are one budget, with the largest lower limit and the smallest upper one. Limits that cross are infeasible, as on the
  solver.
- **Limits.** A finite constant or foldable constant expression, a source column or expression, or an aggregate of the
  source (`<= 0.5*SUM(cost)`, which the right-hand-side rewrite turns into a per-row window value), mixed freely
  across the clauses of one budget. A data-valued limit is reduced per group as S1 reduces its data-valued bound
  (`DirectValidateBounds`): an upper limit takes the group's MIN, a lower one its MAX, and an equality needs one value
  in the group. A NULL or NaN limit raises the solver's error, on every row, including rows outside the `WHEN`. A data
  value is the number it is, however large, as the solver loads it; an infinite one is infinite, so an upper limit of
  +infinity is no bound and a lower limit of +infinity (or an upper one of −infinity) is infeasible.
- **Budget scope.** `PER` gives each group its own budget and its own price. A `WHEN` on the clause or on the aggregate
  (the same meaning for a constant budget) and a NULL `PER` key leave a row out of every budget; that row takes its own
  best end, as a zero-weight row does, and is unbounded when that end is infinite with a nonzero value. Its empty box,
  and a NULL or NaN in any of its values, still raise. A group whose rows all fail the `WHEN` has no budget, as on the
  solver; a `WHEN` that covers no row of a nonempty source raises the empty-aggregate error. Clauses merge into one
  budget only with the same terms, the same `PER` keys in the same order, and equal `WHEN`s.
- **Several decisions in one budget.** Each (row, decision) is an item in the ranking with its own weight, value and
  box, and each decision answers in its own column. A decision the budget does not name has weight 0 and takes its own
  best end. Piece values may shrink or grow (`definition.md` § Correctness).
- **A linear objective.** `MAXIMIZE` or `MINIMIZE` a signed, scaled sum of `SUM(v*x)` parts, or no objective.

The budget is what keeps R1 apart from A1, which admits no aggregate constraint, and the `REAL` decision keeps it apart
from S1, which counts `BOOL` decisions.

## What it does not admit (falls back to the solver)

| Shape | Why |
|---|---|
| An objective with `PER` or `WHEN` | Not a separable value per row. |
| Budget clauses with different scopes (`PER` on one only, other keys or order, different `WHEN`s) | Two budgets have no single price per unit. |
| A decision that is not `REAL`, or not row-scoped | `BOOL` and `INT` cannot fill part of an item. |
| A per-row constraint that reads two decisions, or an objective product of two decisions | It links items, so they are no longer separate in the ranking. |
| Two aggregate constraints with different terms | Two budgets have no single price per unit. |
| A constant limit of magnitude 1e20 or more, or an infinite one | The solver's reading of such a constant depends on the rows (an upper limit of 1e25 beside an uncapped improving row reports unbounded), so the query keeps the solver's reading. A data-valued limit cannot be declined before the rows are read, so it is read as its number (§ Where it differs). |
| A nonlinear term, a product of two decisions, or a coefficient that is not a value of the source row | Not a knapsack weight or value. |
| A membership or not-equal constraint on `x` | Neither a lower nor an upper bound. |

## Where it differs from the solver

- **Rounding on a binding budget.** The plan sums the rooms in `DOUBLE`. A budget that does not bind is exact. A
  binding one that mixes magnitudes far apart (1e19 beside values near 1) can lose the small terms, and the spend can
  exceed `B` by about 1e-13 of its size, inside the solver's own tolerance.
- **A per-row cap of 1e25 under forced HiGHS.** The default solver and direct solve read it as no bound; HiGHS, forced,
  reads it as a number. A1 shares this.
- **A data-valued upper limit between 1e21 and 1e25 beside an uncapped improving row.** Gurobi reports unbounded;
  direct solve returns the finite optimum the limit allows. The problem is bounded, so this is Gurobi's numerics, not
  a different reading. A data-valued upper limit of −infinity beside a floorless row is infeasible here and a status
  error on the solver.
- **An equality limit that varies within a group.** Both paths raise; the wording differs ("varies within a group"
  against the solver's "takes more than one value here").
- **Ties at the price.** The solver returns a vertex: one tied item partly filled, the others at an end. The aggregate
  plan moves every tied item by the same fraction of its room, so the answer is the same on every run. The objective is the
  same.
- **Which error wins.** A NULL or NaN value raises before infeasible or unbounded, wherever the
  row lies, as on the solver; the empty-aggregate error of a `WHEN` that covers no row comes after a data error.
- **Error wording.** Infeasible, unbounded, NULL and NaN errors read as the solver's; `test_direct_user_facing.py`
  compares them in full.

## How it works

`definition.md` § Relational construction and § The plan, built by `AggregatePlanBuilder` for every condition R1
admits. The per-row values of an item (stage 1 of § The plan) come from `ItemStages`: every raw value (coefficient,
bound, mask, data limit) evaluated on every row with its NULL and NaN check, each clause's bounds, the value `v`
(negated under `MINIMIZE`, zero with no objective), the box `[l, u]`, and per decision whether the box is empty, the
anchor, the break-even price `t = v / w` (items with `w = 0` or outside every budget take their best end and enter no
sum), and the finite room up and down. An infinite room is a flag, counted apart, so no sum adds infinity to minus
infinity. `ItemAlgebra` holds the verdicts, the candidate price, the price, the sums at the price and the answer.

`DirectSourceReader` (`direct_source_reader.cpp`) hands out one read of the source per stage. A source that
`DirectRepeatableSourceChain` accepts (non-volatile projections and filters over one repeatable DuckDB table scan) is
rebuilt per read with fresh table indexes, its pushed-down table filters copied; any other source is one
`LogicalMaterializedCTE`, defined through a projection that names its columns (a join under it may reorder its output
in later passes) and read by `LogicalCTERef`s. The group key is the eligibility flag (the budget's `WHEN` holds and no
`PER` key is NULL) and the `PER` keys, joined with IS NOT DISTINCT FROM in a SINGLE join, the form DuckDB uses for a scalar subquery: every
item finds its one group row. A SINGLE join is never flipped by `BUILD_SIDE_PROBE_SIDE` and JOIN_ORDER estimates it as
its item side, so the hash table is always built on the group rows; an inner join would be estimated far larger than
either side and flipped to build on the items. With no scope the aggregates are ungrouped and the joins are cross
products with one row.

1. Group totals: a hash aggregate of every item's sums, the row count, each data limit's MIN and MAX, and the check of
   every raw value and `v`, so a NULL or NaN raises here, before any verdict. An upper data limit takes the group's
   MIN, a lower one its MAX, and an equality both, which must agree in a group with a budget ("varies within a group").
   A limit on a sum with its own `WHEN` is reduced over the budget's rows and the other rows with the same `PER` keys,
   by a window over the group rows. With a scope a window over the group rows counts the eligible groups for the
   empty-aggregate error. A projection raises the empty-aggregate error, then infeasible, then unbounded, and gives the
   stages after it `S`, `L`, `U`, the finite room down, the highest price of an infinite room up and the lowest of one
   down, and which limit binds.
2. The price: the narrow items (group key, `t`, finite room up plus finite room down) go through the plan's one window,
   partitioned by group and ordered by `t` highest first, with one running sum of the two rooms: the most the group can
   spend with the items so far at their high end is `(S - down) + run`. Joined to the totals, each item tests whether
   it fixes the price; where the infinite rooms lie is read from the group's extreme open prices, which agree with
   running counts on the item each test reads. Both tests are monotone along the sort, because rounded sums do not
   land exactly on a limit. A hash aggregate takes the highest or lowest candidate per group.
3. The sums at the price: a hash aggregate over the items joined to the price gives the finite rooms above, below and
   at it and the infinite rooms at it, then the two shares of step 6.
4. The answer: the source rows join their group's numbers, and one projection gives one answer per decision, held to
   its box, and the verdict column, which the adapter keeps so a parent that reads no answer still raises.

With several decisions, each read computes every decision's item values in columns of their own (`t_0`, `t_1`, ...).
Stages 1 and 3 sum over the decisions of a row inside one aggregate input, and the extreme open prices combine with
`greatest` and `least`. Stage 2 unnests only two narrow lists per row, the decisions' prices and their rooms, so the
sort carries the group key, the price, the room and its running sum. Stage 4 gives one answer column per decision.

No row number, breaker, group-back or window over source columns is in the plan, and the row id the driver appends to
a stored source is passed through as a source column.

An empty source returns no rows, whatever the limits are, as on the solver.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_r1_continuous_knapsack.py` | The answers on the independent oracle (`oracle_solver`), the solver path (`off`) and the direct path (`require`): weights of each sign and zero, positive lower bounds, per-row and constant bounds, a lone free row, an improving pair, no objective, `MINIMIZE`, a budget that does not bind, ties; each budget direction binding and not binding, an exact fill reached from above and from below and at a negative price, `BETWEEN` binding at either side, and the minimum-cost cover. A parent query that joins the source, an empty source, a nested `DECIDE`, a parent filter and `LIMIT`, duplicate rows. Sources: a stored table read with a pushed filter, a filter operator, a projection, a join and a `LIMIT`, with and without a scope, against the solver; a volatile source read once; a source with a correlated subquery; a non-table source materialized once. Every hash join builds on the group rows, under `WHEN` and under `PER`. Proportional tie shares with finite and with infinite rooms; a NULL or NaN value raising before a verdict. Several decisions: two with shrinking and with growing piece values, three, a decision outside the budget, mixed signs, `MINIMIZE`, `=`, `PER`, late errors on a second decision; a plan whose one sort and one unnest carry only narrow columns and that has no group-back; tied items of several decisions sharing in proportion to their room. Scoped cases: several `PER` groups at different prices, `WHEN` in both spellings, `PER` with `WHEN`, a NULL key, a group with no covered row, `PER` with `>=` and `BETWEEN`; scopes that differ stay on the solver. Error cases that must fail on both paths (a `WHEN` covering no row, an unbounded row outside the `WHEN`, empty box, unreachable budget or fill, crossed limits, unbounded rows, including an uncapped improving row under `>=`, NULL and NaN values, late bad rows). Boundary cases against the solver only (a `BOOLEAN` column as a weight; an upper data limit of −infinity beside a row with no floor). Data-valued limits: a column, global and per group, an expression, an aggregate, global and with `PER`, under `WHEN`, a lower and an equality limit, a constant and a data clause mixed, two decisions with a column limit; errors for an equality limit that varies in a group and NULL and NaN limits; an equality to a data limit of 1e19, which is a number and not "no bound"; an equality that differs without a scope, and over the rows outside the sum's `WHEN`, raises, and one that differs only among rows outside the `WHEN` does not; a lower limit on a sum with its own `WHEN` takes the maximum over the rows outside it too, and is infeasible when that maximum is out of reach; a data limit, global and per group, with one and two decisions, runs the plan of a constant limit. Constant limits the solver reads by the rows stay on the solver. The plan has one sort and no solver operator; on the aggregate plan the sort carries only narrow columns (checked with twenty unused source columns), a stored table is read four times, and a scoped budget adds hash aggregates and joins. |
| `test_direct_rule_conformance.py` | R1's fixture: the checks every rule owes, applying forms with `>=`, `=` and one sum compared twice, and a two-decision form, a column budget, an aggregate budget, and near misses that must decline (a per-row constraint linking two decisions, an `INT` decision, two budgets with different weights, a nonlinear objective term). |
| `test_direct_user_facing.py` | R1's NULL, NaN, infinite and overflow errors name the right clause, and the infeasible and unbounded text match the solver's, compared in full. |
| C++ `[decidb]` | R1's applicability condition: each decline branch, and the forms that apply; R1 never applies where S1 or A1 does; the rule driver reaches R1, S1 and A1 each for their own shapes. |

## Performance

Measured 2026-10-23 on top of `aa9ee8fe6b` with this rule's changes in the working tree, on this machine, with
`benchmark/decide/profile_direct_r1.py`. Each run is a fresh `decidb` process that opens the stored database; peak
memory is the process's own. Direct 3 runs (median), each solver 1 run; the solver path is `off` with
`DECIDB_FORCE_SOLVER` set to `gurobi` or `highs`. The parent reads one aggregate row, so the time is the plan, not the
transfer. Before timing, every shape's objective on direct and on Gurobi agreed within 1e-6.

| Shape, 5,000,000 rows | Direct | Direct peak | Gurobi | Gurobi peak | HiGHS | HiGHS peak | vs Gurobi | vs HiGHS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `<=` budget, positive weights | 8.27 s | 3.0 GiB | 7.20 s | 3.3 GiB | 24.0 s | 4.6 GiB | 0.9x | 2.9x |
| `<=` budget, weights of mixed sign | 8.02 s | 4.0 GiB | 5.75 s | 3.8 GiB | 9.32 s | 4.7 GiB | 0.7x | 1.2x |
| `>=` cover, `MINIMIZE` | 7.52 s | 4.0 GiB | 7.04 s | 3.7 GiB | 23.7 s | 4.5 GiB | 0.9x | 3.2x |
| `=` budget | 7.43 s | 4.0 GiB | 7.10 s | 3.7 GiB | 24.5 s | 4.5 GiB | 1.0x | 3.3x |
| `BETWEEN` budget | 7.37 s | 4.0 GiB | 8.07 s | 3.9 GiB | 27.5 s | 3.2 GiB | 1.1x | 3.7x |
| `PER` 1,000 groups | 3.39 s | 3.6 GiB | 9.36 s | 4.1 GiB | 31.0 s | 4.4 GiB | 2.8x | 9.1x |
| `WHEN` on the budget | 5.14 s | 4.7 GiB | 6.16 s | 4.6 GiB | 17.7 s | 4.7 GiB | 1.2x | 3.4x |
| two decisions | 58.0 s | 4.0 GiB | 14.9 s | 4.2 GiB | 46.6 s | 5.9 GiB | 0.3x | 0.8x |
| budget from a column, `PER` | 3.40 s | 4.0 GiB | 8.89 s | 5.7 GiB | 31.0 s | 5.6 GiB | 2.6x | 9.1x |
| budget from an aggregate | 10.8 s | 5.4 GiB | 7.53 s | 4.8 GiB | 24.3 s | 5.5 GiB | 0.7x | 2.2x |
| `<=` budget, parent reads only `COUNT(*)` | 6.22 s | 3.1 GiB | 6.76 s | 5.6 GiB | 23.6 s | 5.5 GiB | 1.1x | 3.8x |

At 1,000,000 rows the ratios are the same (0.2x to 3.8x against Gurobi, 0.8x to 9.4x against HiGHS), direct peaks at
0.6 to 1.6 GiB and Gurobi at 1.4 to 2.3 GiB.

**Where the time goes.** From DuckDB's JSON profiler: the operator times at 1,000,000 rows, and the window widths,
planning time, spill and two-decision run at 100,000 rows from `benchmark/decide/profile_r1_plan.py` (the widths and the
plan do not depend on the row count). At 1,000,000 rows (`le_positive`, all threads, 1.52 s) the operators use 1.37 s of CPU: the window that sorts 0.81 s,
the four windows without an order 0.39 s together, and eighteen projections under 0.2 s together. The query keeps
about one core busy (1.38 s of CPU in 1.52 s). The parent reads only `COUNT(*)` and `SUM(x)`, so the optimizer drops
the source columns the plan does not read; the four largest windows each carry 24 to 36 columns, of which one is a
source column and the rest are the plan's own stage columns, which every stage forwards to the next. The floor's one
sort carries 3. Planning and optimizing take under 5 ms, and no operator writes to the temporary directory. For one
global budget the plan holds 5 windows, one of which sorts and none of which is partitioned, and no join. With two decisions at 100,000 rows the
query runs 1.10 s on 1.32 s of CPU without the profiler: about one core busy, and about 11 s per million rows against
1.5 s for one decision, so the cost is the plan's serial work, not a wait. Profiled with `EXTRA_INFO` on, the same
query runs 1.15 s. The profiler renders each operator's EXPLAIN text once, after execution, from the operator's final
state (`QueryProfiler::RenderExtraInfo`, called from `EndQuery` and from `EXPLAIN ANALYZE`'s finalize). Rendered per
chunk, as stock DuckDB does, the same query runs 5.9 s on 11.6 s of CPU: the plan's projections hold up to 6.7 KB of
nested `CASE` text each, and that work runs outside the operator timers, so the profile's `cpu_time` does not show it.
The operator times and latencies above were taken with the per-chunk render, so their latencies are inflated by it.

**Floor and scaling.** Measured 2026-10-23 on top of `4e34b663f5` with `benchmark/decide/profile_r1_floor.py`, on a
machine with 11 cores and a background load average of 2 to 6 from other applications. Medians of 3 interleaved runs;
for runs over 1 s min and max lie within 10% of the median, and the floor's sub-second runs vary more (0.46 to 0.69 s
at 5,000,000 rows, all threads). The floor is a hand-written DuckDB query of the narrow shape for
`le_positive`: keys, `t = v / w` and rooms; one sort by `t`; a running sum; a join back that gives every row its
answer. It returns the same objective and `SUM(x)` as the direct plan at every size.

| `le_positive` | Rows | 1 thread | All threads | CPU / wall (all) | Peak (all) |
|---|---:|---:|---:|---:|---:|
| Floor | 1M | 0.29 s | 0.11 s | 4.2 | 218 MiB |
| Floor | 2M | 0.62 s | 0.23 s | 5.2 | 431 MiB |
| Floor | 5M | 1.56 s | 0.51 s | 5.0 | 1.2 GiB |
| Direct | 1M | 1.41 s | 1.43 s | 1.7 | 731 MiB |
| Direct | 2M | 2.89 s | 2.96 s | 1.7 | 1.5 GiB |
| Direct | 5M | 7.41 s | 7.74 s | 2.0 | 3.8 GiB |
| Gurobi | 1M | | 1.17 s | 1.05 | 1.4 GiB |
| Gurobi | 2M | | 2.80 s | 1.01 | 2.3 GiB |
| Gurobi | 5M | | 7.46 s | 1.02 | 3.3 GiB |

A bare scan that returns one `DOUBLE` per row takes 0.02 to 0.03 s at every size. Gurobi's time is its solver call;
model construction is 2% to 3% of the wall time at every size, and everything outside the solver call together
(model construction, process start, planning, reading and returning rows) is under 7%.

With one global budget the direct plan runs at Gurobi's speed and about 15 times the floor at 5,000,000 rows. At one
thread it does about 4.8 times the floor's work (7.41 s against 1.56 s), and unlike the floor it gains nothing from
more threads (the floor runs 3.1 times faster at 5,000,000 rows). The floor beats Gurobi at every size, with one
thread too (1.56 s against 7.46 s). Time grows about linearly in rows for the floor, the direct plan and Gurobi (5M
against 1M: 4.6, 5.4 and 6.4 times).

Threads help the direct plan when the budget is split into many partitions: one thread against all threads at
1,000,000 rows, `per_groups` 1.68 s to 0.69 s and `when_budget` 1.75 s to 0.97 s; `two_decisions` 11.21 s to 11.00 s.
At 5,000,000 rows `per_groups` runs 9.16 s to 3.76 s (Gurobi 9.74 s). The plan's time on one global budget is the
plan's, not the method's: the floor shows the method needs a small fraction of it.

**Reproduce** (after `make release`; the sweep took about 20 minutes, building the databases included):

```sh
python3 benchmark/decide/profile_direct_r1.py --rows 1000000 5000000 --repeats 3 --solver-budget 60 --db-dir build/r1db
```

The floor and scaling table (about 10 minutes at 1,000,000 rows; the larger sizes and `two_decisions` at one thread add
much more):

```sh
python3 benchmark/decide/profile_r1_floor.py --rows 1000000 --repeats 3 --floor-threads 1 2 4 all --db-dir build/r1db
python3 benchmark/decide/profile_r1_floor.py --rows 2000000 5000000 --shapes le_positive per_groups --threads 1 all \
    --floor-threads 1 all --repeats 3 --db-dir build/r1db
```

The operator profile (one run per cell; `--variants` and `--repeats` widen it):

```sh
python3 benchmark/decide/profile_r1_plan.py --rows 1000000 --shapes le_positive two_decisions --threads 1 all \
    --db-dir build/r1db_plan
```

