# R1 (continuous knapsack) — what works today

Verified on 2026-10-29 at `c85704ad45`: `make decide-test`, `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`,
`DECIDB_VERIFY_SERIALIZER=1 make decide-test` and `DECIDB_FORCE_SOLVER=highs make decide-test` each 5,194 passed; C++
`build/release/test/unittest "[decidb]"` 2,869 assertions in 69 cases.
`test/decide/run_direct_matrix.sh` renews these numbers in one command (about 3 minutes; it prints one line per check).
Code: `src/optimizer/decide/direct/r1_continuous_knapsack_rule.cpp`; its per-row bounds and the linear terms of the
budget and the objective come from the shared reader in `direct_bound_reader.cpp`. Class definition and correctness
argument: [definition.md](definition.md).

## What it admits

One or more row-scoped `REAL` decisions with:

- **Bounds, per row.** Every per-row bound the shared reader reads for A1: constants, columns and expressions, with a
  coefficient, an offset and a `when` on the clause. Lower bounds may be positive or negative. A bound of magnitude
  1e20 or more is no bound, as on the solver, so a row may be uncapped, floorless or free.
- **One budget, `L <= SUM(w*x) <= U`.** A plain `SUM` of linear terms in `x`. The weight `w` is a column or
  expression of any sign, read per row, and zero on some rows is allowed. The budget is written `<=`, `>=`, `=` or
  `BETWEEN`, or as the same sum compared several times: aggregate constraints with the same terms in the same order
  are one budget, with the largest lower limit and the smallest upper one. Limits that cross are infeasible, as on the
  solver.
- **Limits.** A finite constant or foldable constant expression, a source column or expression, or an aggregate of the
  source (`<= 0.5*SUM(cost)`), mixed freely across the clauses of one budget. A data-valued limit reaches R1 as a group
  bound (`../../framework/done.md`): R1 adds its aggregates (the MIN of an upper limit, the MAX of a lower one, an
  equality's MIN and MAX that must agree, or a reducer of the budget's group) to the hash aggregate it already runs, and
  raises its NULL, NaN and disagreement errors under the plan, in the solver's words. A plain data limit takes the
  same path. A row the `when` drops is not read (syntax reference §6.3). A data
  value is the number it is, however large, as the solver loads it; an infinite one is infinite, so an upper limit of
  +infinity is no bound and a lower limit of +infinity (or an upper one of −infinity) is infeasible.
- **Budget scope.** One budget over the source. A `when` on the clause or inside the sum (the same meaning for a
  constant budget), or one in each place (the budget then covers the rows both keep), leaves a row out of the budget; that row takes its own best end, as a zero-weight row does, and is
  unbounded when that end is infinite with a nonzero value. Its empty box, and a NULL or NaN in its own bounds, still
  raise; its weight is not read. A `when` that keeps no row, or a sum whose own `when` keeps none, leaves no budget,
  so every row takes its own best end, as on the solver (D68). Clauses merge into one budget only with the same terms and
  equal memberships. A `by` key on the sum gives each group its own budget
  and price; a NULL key is in no group (D5). A `per K:` clause bounds the whole budget once per class (D6). Clauses
  merge into one budget only with the same key; the same key written in two orders is one key.
- **Several decisions in one budget.** Each (row, decision) is an item in the ranking with its own weight, value and
  box, and each decision answers in its own column. A decision the budget does not name has weight 0 and takes its own
  best end. Piece values may shrink or grow (`definition.md` § Correctness).
- **A linear objective.** `MAXIMIZE` or `MINIMIZE` a signed, scaled sum of `SUM(v*x)` parts, or no objective.

The budget is what keeps R1 apart from A1, which admits no aggregate constraint, and the `REAL` decision keeps it apart
from S1, which counts `BOOL` decisions.

## What it does not admit (falls back to the solver)

| Shape | Why |
|---|---|
| An objective with a `when`, a `per` or a two-level reducer | Not a separable value per row. |
| Budget clauses with different `when`s | Two budgets have no single price per unit. |
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
- **A weight near the DOUBLE limit.** `SUM(1e308*x) <= 1` with `x` in [0, 5]: the solver path warns and solves; R1's
  rooms `w·(high − a)` overflow, and direct solve raises TOO_LARGE naming the budget.
- **A per-row cap of 1e25 under forced HiGHS.** The default solver and direct solve read it as no bound; HiGHS, forced,
  reads it as a number. A1 shares this.
- **A data-valued upper limit between 1e21 and 1e25 beside an uncapped improving row.** Gurobi reports unbounded;
  direct solve returns the finite optimum the limit allows. The problem is bounded, so this is Gurobi's numerics, not
  a different reading. A data-valued upper limit of −infinity beside a floorless row is infeasible here and a status
  error on the solver.
- **Ties at the price.** The solver returns a vertex: one tied item partly filled, the others at an end. The aggregate
  plan moves every tied item by the same fraction of its room, so the answer is the same on every run. The objective is the
  same.
- **Which error wins.** A NaN value raises before infeasible or unbounded, wherever the row lies, as on the solver.
- **Error wording.** Infeasible, unbounded and NaN errors read as the solver's; `test_direct_user_facing.py`
  compares them in full.
- **One unexplained garbage result.** Once, in a full `DECIDB_VERIFY_SERIALIZER=1 make decide-test`,
  `test_correlated_subquery_is_real` (its source is materialized as a `LogicalMaterializedCTE`) returned an `l_orderkey`
  near 4.6e18 and an `x` near 1e-317. The values look like one type's bits read as another. It has not recurred: not in
  40 runs of the test alone, 10 parallel runs with every R1 test, or three full serializer suites, and not in 900
  runs of its query on a Debug build with AddressSanitizer and UndefinedBehaviorSanitizer (300 with the serializer
  check, 300 without, 300 concurrent with it; the plan shows the materialized CTE and its scan). Serialization of
  `LogicalCTERef` (table and CTE index, chunk types, bound columns, materialize flag) and `LogicalMaterializedCTE`
  (table index, column count, name) round-trips what the plan needs. No fault is known.

## How it works

`definition.md` § Relational construction and § The plan, built by `AggregatePlanBuilder` for every condition R1
admits. The per-row values of an item (stage 1 of § The plan) come from `ItemStages`: every raw value (coefficient,
bound, mask, data limit) evaluated on the rows that read it, with its NULL and NaN check (a row a `when` drops reads
nothing), each clause's bounds, the value `v`
(negated under `MINIMIZE`, zero with no objective), the box `[l, u]`, and per decision whether the box is empty, the
anchor, the break-even price `t = v / w` (items with `w = 0` or outside every budget take their best end and enter no
sum), and the finite room up and down. An infinite room is a flag, counted apart, so no sum adds infinity to minus
infinity. `ItemAlgebra` holds the verdicts, the candidate price, the price, the sums at the price and the answer.

`DirectSourceReader` (`direct_source_reader.cpp`) hands out one read of the source per stage (`SOURCE_READS`, 4). A
source that `DirectRepeatableSourceChain` accepts (non-volatile projections and filters over one repeatable DuckDB table
scan) is copied once when `DirectSourceCopyIsCheaper` says so, else rebuilt per read with fresh table indexes, its
pushed-down table filters copied. A read of the copy costs `COPY_READ_COST` (0.4) of a re-scan, so a source read three
times or more is copied, provided the copy (estimated rows times the source's row width) is at most
`1/COPY_MEMORY_SHARE` (1/8) of the memory limit; with no row estimate it is re-scanned. Both constants come from
interleaved timings on partsupp and a 20-column wide table at 10K to 1M rows: at 100K the copy took 68 ms against 96
(narrow) and 66 against 94 (wide), and past an eighth of the memory limit it lost. At 10K rows on partsupp it is 14 ms
slower, in R1's sort, not in the reads. Any other source is one
`LogicalMaterializedCTE`, defined through a projection that names its columns (a join under it may reorder its output
in later passes) and read by `LogicalCTERef`s. The group key is the eligibility flag (the budget's `when` holds) and
the `by` keys, joined with IS NOT DISTINCT FROM in a SINGLE join, the form DuckDB uses for a scalar subquery: every
item finds its one group row. A SINGLE join is never flipped by `BUILD_SIDE_PROBE_SIDE` and JOIN_ORDER estimates it as
its item side, so the hash table is always built on the group rows; an inner join would be estimated far larger than
either side and flipped to build on the items. With no scope the aggregates are ungrouped and the joins are cross
products with one row.

1. Group totals: a hash aggregate of every item's sums, the row count, each data limit's MIN and MAX, and the check of
   every raw value and `v`, so a NaN raises here, before any verdict; a NULL weight or value leaves its row out of the
   sums, and a NULL limit is no limit. A data limit is one value on the group's
   rows, so its MIN (upper) or MAX (lower) carries it. A projection raises the data checks, then infeasible, then unbounded, and
   gives the
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
`greatest` and `least`. Stage 3 unpivots only the narrow items: the row, with each decision's price and room in columns of
its own, is crossed with a constant relation of the decision numbers, and a projection picks the price and room of the
decision a number names. That costs a fraction of DuckDB's `UNNEST` of two lists (10,000,000 items took 0.03 s in
isolation against 0.85 s), which the plan used before. The sort carries the group key, the price, the room and its
running sum. Stage 4 gives one answer column per decision.

No row number, breaker, group-back or window over source columns is in the plan, and the row id the driver appends to
a stored source is passed through as a source column.

An empty source returns no rows, whatever the limits are, as on the solver.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_r1_continuous_knapsack.py` | The answers on the independent oracle (`oracle_solver`), the solver path (`off`) and the direct path (`require`): weights of each sign and zero, positive lower bounds, per-row and constant bounds, a lone free row, an improving pair, no objective, `MINIMIZE`, a budget that does not bind, ties; each budget direction binding and not binding, an exact fill reached from above and from below and at a negative price, `BETWEEN` binding at either side, and the minimum-cost cover. A parent query that joins the source, an empty source, a nested `DECIDE`, a parent filter and `LIMIT`, duplicate rows. Sources: a stored table read with a pushed filter, a filter operator, a projection, a join and a `LIMIT`, with and without a scope, against the solver; a volatile source read once; a source with a correlated subquery; a non-table source materialized once. Every hash join builds on the group rows, under `when` and under `by`. Proportional tie shares with finite and with infinite rooms; a NULL or NaN value raising before a verdict. Several decisions: two with shrinking and with growing piece values, three, a decision outside the budget, mixed signs, `MINIMIZE`, `=`, `by`, late errors on a second decision; a plan whose one sort and one unnest carry only narrow columns and that has no group-back; tied items of several decisions sharing in proportion to their room. Scoped cases: several `by` groups at different prices, `when` in front of the clause, inside the sum and in both places, `by` with `when`, a NULL key, a group with no covered row, `by` with `>=` and `BETWEEN`; scopes that differ stay on the solver. Error cases that must fail on both paths (a `when` keeping no row, a sum's own `when` keeping none of the clause's rows, an unbounded row outside the `when`, empty box, unreachable budget or fill, crossed limits, unbounded rows, including an uncapped improving row under `>=`, NULL and NaN values, late bad rows). Boundary cases against the solver only (a `BOOLEAN` column as a weight; an upper data limit of −infinity beside a row with no floor). Data-valued limits: a column, global and per group, an expression, an aggregate, global and with `by`, under `when`, a lower and an equality limit, a constant and a data clause mixed, two decisions with a column limit; errors for an equality limit that varies in a group and NULL and NaN limits; an equality to a data limit of 1e19, which is a number and not "no bound"; an equality that differs without a scope, and over the rows outside the sum's `when`, raises, and one that differs only among rows outside the `when` does not; a lower limit on a sum with its own `when` takes the maximum over the rows outside it too, and is infeasible when that maximum is out of reach; a data limit, global and per group, with one and two decisions, runs the plan of a constant limit. Constant limits the solver reads by the rows stay on the solver. The plan has one sort and no solver operator; on the aggregate plan the sort carries only narrow columns (checked with twenty unused source columns), a stored table is read four times, and a scoped budget adds hash aggregates and joins. The direct path of a case with a `by` key is a strict xfail (`direct_path`), and the grouped plan-shape tests are `xfail(strict=True)`, until R1 reads a `by` key (`todo.md`). |
| `test_direct_rule_conformance.py` | R1's fixture: the checks every rule owes, applying forms with `>=`, `=` and one sum compared twice, and a two-decision form, a column budget, an aggregate budget, and near misses that must decline (a per-row constraint linking two decisions, an `INT` decision, two budgets with different weights, a nonlinear objective term). |
| `test_direct_user_facing.py` | R1's NULL, NaN, infinite and overflow errors name the right clause, and the infeasible and unbounded text match the solver's, compared in full. |
| C++ `[decidb]` | R1's applicability condition: each decline branch, and the forms that apply; R1 never applies where S1 or A1 does; the rule driver reaches R1, S1 and A1 each for their own shapes. |

## Performance

Measured 2026-10-24 on top of `5269ad7c5e`, on a machine with 11 cores, with `benchmark/decide/direct_solve/archive/profile_direct_r1.py`.
The per-group rows were measured on the per-group plan before R1 read a `by` key from a query, and are not measured since.
Each run is a fresh `decidb` process that opens the stored database; peak memory is the process's own. Direct 3 runs
(median), each solver 1 run; the solver path is `off` with `DECIDB_FORCE_SOLVER` set to `gurobi` or `highs`. The parent
reads one aggregate row, so the time is the plan, not the transfer. Before timing, every shape's objective on direct and on Gurobi agreed within 1e-6.

| Shape, 5,000,000 rows | Direct | Direct peak | Gurobi | Gurobi peak | HiGHS | HiGHS peak | vs Gurobi | vs HiGHS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `<=` budget, positive weights | 0.63 s | 0.9 GiB | 7.37 s | 3.8 GiB | 24.0 s | 4.5 GiB | 11.7x | 38.1x |
| `<=` budget, weights of mixed sign | 0.65 s | 0.9 GiB | 5.80 s | 3.7 GiB | 8.98 s | 4.7 GiB | 8.9x | 13.7x |
| `>=` cover, `MINIMIZE` | 0.62 s | 0.8 GiB | 7.06 s | 4.6 GiB | 24.0 s | 4.7 GiB | 11.4x | 38.6x |
| `=` budget | 0.70 s | 0.9 GiB | 7.10 s | 4.4 GiB | 24.7 s | 4.7 GiB | 10.1x | 35.0x |
| `BETWEEN` budget | 0.63 s | 0.9 GiB | 8.15 s | 4.3 GiB | 26.1 s | 4.4 GiB | 13.0x | 41.6x |
| per-group, 1,000 groups | 0.58 s | 0.4 GiB | 9.33 s | 4.6 GiB | 31.6 s | 4.5 GiB | 16.1x | 54.6x |
| `WHEN` on the budget | 0.73 s | 0.9 GiB | 6.23 s | 4.3 GiB | 18.2 s | 4.6 GiB | 8.5x | 24.9x |
| two decisions | 1.36 s | 1.6 GiB | 14.7 s | 4.7 GiB | 46.9 s | 5.7 GiB | 10.8x | 34.4x |
| budget from a column, per group | 0.61 s | 0.4 GiB | 8.96 s | 5.8 GiB | 31.5 s | 5.6 GiB | 14.7x | 51.5x |
| budget from an aggregate | 1.08 s | 1.4 GiB | 7.46 s | 4.5 GiB | 24.5 s | 5.4 GiB | 6.9x | 22.7x |
| `<=` budget, parent reads only `COUNT(*)` | 0.55 s | 0.9 GiB | 6.75 s | 5.6 GiB | 23.3 s | 5.5 GiB | 12.3x | 42.5x |

At 1,000,000 rows direct runs 0.13 to 0.35 s (4.5x to 16.5x Gurobi, 9.0x to 41.2x HiGHS) and peaks at 0.2 to 0.4 GiB,
against 1.4 to 2.2 GiB for Gurobi. Every shape beats both solvers at both sizes.

**Floor and scaling.** Measured with `benchmark/decide/direct_solve/archive/profile_r1_floor.py` in the same session; medians of 3
interleaved runs. The floor is a hand-written DuckDB query of the narrow shape for `le_positive`: keys, `t = v / w` and
rooms; one sort by `t`; a running sum; a join back that gives every row its answer. It returns the same objective and
`SUM(x)` as the direct plan at both sizes.

| Rows | Method | Shape | 1 thread | All threads | CPU / wall (all) | Peak (all) |
|---|---|---|---:|---:|---:|---:|
| 1M | floor | `le_positive` | 0.30 s | 0.11 s | 4.6 | 217 MiB |
| 1M | direct | `le_positive` | 0.61 s | 0.16 s | 6.0 | 178 MiB |
| 1M | direct | `per_groups` | 0.71 s | 0.16 s | 6.5 | 168 MiB |
| 1M | direct | `two_decisions` | 1.24 s | 0.35 s | 5.6 | 403 MiB |
| 1M | Gurobi | `le_positive` | | 1.20 s | 1.06 | 1.4 GiB |
| 5M | floor | `le_positive` | 1.62 s | 0.51 s | 5.3 | 1.2 GiB |
| 5M | direct | `le_positive` | 3.04 s | 0.60 s | 8.2 | 873 MiB |
| 5M | direct | `per_groups` | 3.58 s | 0.61 s | 8.3 | 391 MiB |
| 5M | direct | `two_decisions` | 6.04 s | 1.31 s | 8.2 | 1.7 GiB |
| 5M | Gurobi | `le_positive` | | 7.15 s | 1.02 | 4.8 GiB |
| 5M | Gurobi | `per_groups` | | 9.47 s | 3.66 | 5.3 GiB |
| 5M | Gurobi | `two_decisions` | | 14.73 s | 1.00 | 5.0 GiB |

**Where the time goes.** On one budget the plan runs 1.2 times the floor with all threads (0.60 s against 0.51 s at
5,000,000 rows) and about twice its work on one thread (3.04 s against 1.62 s): it reads the source four times, and each
read evaluates the per-row checks and item values the floor does not have. It keeps 6 to 8 cores busy. `PER` costs no
more than one budget. Two decisions run about 2 times one decision at 5,000,000 rows (1.8 to 2.2 times over repeated
runs with all threads, 2.03 on one thread, 2.1 times the CPU: 10.5 s against 4.9 s), because the one sort holds
10,000,000 items and each read computes both decisions' item values. The cross-product unpivot of stage 3 brought this
down from 4.2 times; what remains is the work of twice the items. The `two_decisions` rows of this table were measured
on 2026-10-24 on top of `99488c2b04` with the cross-product unpivot, three runs each. A budget from an aggregate adds the
right-hand-side rewrite's window over the source, which also makes the source a materialized CTE.

**Reproduce** (after `make release`; the sweep takes about 20 minutes, building the databases included):

```sh
python3 benchmark/decide/direct_solve/archive/profile_direct_r1.py --rows 1000000 5000000 --repeats 3 --solver-budget 60 --db-dir build/r1db
```

The floor and scaling table (about 8 minutes):

```sh
python3 benchmark/decide/direct_solve/archive/profile_r1_floor.py --rows 1000000 5000000 --shapes le_positive per_groups two_decisions \
    --methods floor direct gurobi --threads 1 all --floor-threads 1 all --repeats 3 --db-dir build/r1db
```

The operator profile (one run per cell; `--variants` and `--repeats` widen it):

```sh
python3 benchmark/decide/direct_solve/archive/profile_r1_plan.py --rows 1000000 --shapes le_positive two_decisions --threads 1 all \
    --db-dir build/r1db_plan
```

