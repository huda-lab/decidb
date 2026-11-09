# R1 (continuous knapsack) — what works today

Verified on 2026-11-08 at `73b52ab1d1`: `test/decide/run_direct_matrix.sh`, each of its four suites (auto, off,
serializer, highs) 7,020 passed; C++ `build/release/test/unittest "[decidb]"` 4,635 assertions in 85 cases. Code:
`src/optimizer/decide/direct/r1_continuous_knapsack_rule.cpp`; its per-row bounds and the linear terms of the budget and
the objective come from the shared reader in `direct_bound_reader.cpp`. Class definition and correctness argument:
[definition.md](definition.md).

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
- **A linear objective.** `MAXIMIZE` or `MINIMIZE` a signed, scaled sum of `SUM(v*x)` parts, or no objective. The
  objective may have a `when` in front, and each `SUM` one inside (`MAXIMIZE when f: SUM(v*x)`, `MAXIMIZE SUM(when f:
  v*x)`), each a deterministic source predicate that cannot raise. A row they drop (false or NULL) has the value 0 in
  that part and reads no value there, so a NaN or a value that raises on it does not raise; the row keeps its weight
  in the budget. A `when` that keeps no row leaves every value 0.

The budget is what keeps R1 apart from A1, which admits no aggregate constraint, and the `REAL` decision keeps it apart
from S1, which counts `BOOL` decisions.

## What it does not admit (falls back to the solver)

| Shape | Why |
|---|---|
| An objective with a `per` or a two-level reducer | Not a separable value per row. |
| An objective `when` that is volatile or can raise | Not a predicate the plan can read on every read of the source. |
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
- **Its own warning.** The divisor is an item's weight `w`: R1 warns on its own ratio, the price `v / w` with `v` not 0
  (the framework's [Number-range warning](../../framework/done.md)). The ranking reads the prices, and there it loses
  precision: values of 5e-310 and 1e-310 over weights of 2e19 and 3e19 answer 0 and 0 where the true answer is 1 and
  1/3. Values of 5e-280 and 1e-280 keep their prices in range, answer 1 and 1/3 and are silent, where `off` warns and
  returns 0 and 0 (Gurobi) or 0 and 1 (HiGHS).
- **Ties at the price.** The solver returns a vertex: one tied item partly filled, the others at an end. The aggregate
  plan moves every tied item by the same fraction of its room, so the answer is the same on every run. The objective is the
  same.
- **Which error wins.** A NaN value raises before infeasible or unbounded, wherever the row lies, as on the solver.
- **Error wording.** Infeasible, unbounded and NaN errors read as the solver's; `test_direct_user_facing.py`
  compares them in full.
- **One unexplained garbage result.** One full `DECIDB_VERIFY_SERIALIZER=1 make decide-test` run of
  `test_correlated_subquery_is_real` (its source is a `LogicalMaterializedCTE`) returned an `l_orderkey` near 4.6e18 and
  an `x` near 1e-317, as if one type's bits were read as another. It does not reproduce: 40 runs of the test alone, 10
  parallel runs with every R1 test, three full serializer suites, and 900 runs of its query on a Debug build with
  AddressSanitizer and UndefinedBehaviorSanitizer (with and without the serializer check, and concurrently). The
  serialization of `LogicalCTERef` and `LogicalMaterializedCTE` round-trips what the plan needs. No fault is known.

## How it works

`definition.md` § Relational construction and § The plan, built by `AggregatePlanBuilder` for every condition R1
admits. The per-row values of an item (stage 1 of § The plan) come from `ItemStages`: every raw value (coefficient,
bound, mask, data limit) evaluated on the rows that read it, with its NULL and NaN check (a row a `when` drops reads
nothing), each clause's bounds, the value `v` (negated under `MINIMIZE`, zero with no objective, and zero in a part
whose `when` drops the row), the box `[l, u]`, and per decision whether the box is empty, the anchor, the break-even price `t = v / w` (items with `w = 0` or outside every budget take their best end and enter no
sum), and the finite room up and down. An infinite room is a flag, counted apart, so no sum adds infinity to minus
infinity. `ItemAlgebra` holds the verdicts, the candidate price, the price, the sums at the price and the answer.

`DirectSourceReader` (`direct_source_reader.cpp`) hands out one read of the source per stage (`SOURCE_READS`, 4); when
it copies the source and when it re-scans it is in [framework/done.md](../../framework/done.md) § Row checks. The four
reads compute the same item values, so they are one row stage of the reader (`SetRowStage`, `ReadStaged`), not four
computations. When the source is copied, the stage is a materialized common subexpression (a spool): the definition of
a CTE named `direct_stage`, computed once, which each read references, so the algebra of § stage 1 runs once however
many reads there are, and each reference is narrowed to the columns its stage uses. When no other read takes the
source, the copy would be read once, by the stage, so the stage reads the source as itself and the plan holds only
the stage. When the source is re-scanned, each read builds the stage on its own scan.
The
SINGLE join of items to group rows (`definition.md` § The plan; `DirectJoinClassAnswers`, which A2 and N1 share) is never flipped by `BUILD_SIDE_PROBE_SIDE`, and
JOIN_ORDER estimates it as its item side, so the hash table is always built on the group rows; an inner join would be
estimated far larger than either side and flipped to build on the items.

A group's key is the framework's [class key](../../framework/done.md): the `by` keys, each compared by what a NULL means
in it, and one marker column when a `when` or a `per K:` clause with no instance can drop an item and no element says
so. An item in no budget has a NULL in each column of the key compared with `=`, so it meets no group's row and takes
its best end. No BOOLEAN column stands in the key.

2. Group totals: a hash aggregate of every item's sums, the row count, each data limit's MIN and MAX, and the check of
   every raw value and `v`, so a NaN raises here, before any verdict; a NULL weight or value leaves its row out of the
   sums, and a NULL limit is no limit. A check's message is a column of stage 1 that holds a code of its text, not the
   text (dictionary encoding, as the framework's [message codes](../../framework/done.md) § Runtime checks): the
   aggregate takes the `MIN` of the codes, which is the code of the least text because the codes follow the order of
   the texts, and the projection that raises decodes the least code of each check on the group's row
   (`DirectDecodeMessage`) and raises it in the solver's order. Two rows of a group that fail the same check in
   different ways raise the least text, whichever row comes first. The aggregate compares integers per row, and a row
   that passes carries no text. A data limit is one value on the group's rows, so its MIN (upper) or MAX
   (lower) carries it. A projection raises the data checks, then infeasible, then unbounded, and gives the stages after
   it `S`, `L`, `U`, the finite room down, the highest price of an infinite room up and the lowest of one
   down, and which limit binds.
3. The price: the narrow items (group key, `t`, finite room up plus finite room down) go through the plan's one window,
   partitioned by group and ordered by `t` highest first, with one running sum of the two rooms: the most the group can
   spend with the items so far at their high end is `(S - down) + run`. Joined to the totals, each item tests whether
   it fixes the price; where the infinite rooms lie is read from the group's extreme open prices, which agree with
   running counts on the item each test reads. Both tests are monotone along the sort, because rounded sums do not
   land exactly on a limit. A hash aggregate takes the highest or lowest candidate per group.
4. The sums at the price: a hash aggregate over the items joined to the price gives the finite rooms above, below and
   at it and the infinite rooms at it, then the two shares of step 6.
5. The answer: the source rows join their group's numbers, and one projection gives one answer per decision, held to
   its box, and the verdict column, which the adapter keeps so a parent that reads no answer still raises.

With several decisions, each read computes every decision's item values in columns of their own (`t_0`, `t_1`, ...).
Stages 2 and 4 sum over the decisions of a row inside one aggregate input, and the extreme open prices combine with
`greatest` and `least`. Stage 3 unpivots only the narrow items: the row, with each decision's price and room in columns of
its own, is crossed with a constant relation of the decision numbers, and a projection picks the price and room of the
decision a number names. That costs a fraction of DuckDB's `UNNEST` of two lists (10,000,000 items took 0.03 s in
isolation against 0.85 s). The sort carries the group key, the price, the room and its
running sum. Stage 5 gives one answer column per decision.

No row number, breaker, group-back or window over source columns is in the plan, and the row id the driver appends to
a stored source is passed through as a source column.

An empty source returns no rows, whatever the limits are, as on the solver.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_r1_continuous_knapsack.py` | The answers on the independent oracle (`oracle_solver`), the solver path (`off`) and the direct path (`require`): weights of each sign and zero, positive lower bounds, per-row and constant bounds, a lone free row, an improving pair, no objective, `MINIMIZE`, a budget that does not bind, ties; each budget direction binding and not binding, an exact fill reached from above and from below and at a negative price, `BETWEEN` binding at either side, and the minimum-cost cover. A parent query that joins the source, an empty source, a nested `DECIDE`, a parent filter and `LIMIT`, duplicate rows. Sources: a stored table read with a pushed filter, a filter operator, a projection, a join and a `LIMIT`, with and without a scope, against the solver; a volatile source read once; a source with a correlated subquery; a non-table source materialized once. Every hash join builds on the group rows, under `when` and under `by`. Proportional tie shares with finite and with infinite rooms; a NULL or NaN value raising before a verdict. Several decisions: two with shrinking and with growing piece values, three, a decision outside the budget, mixed signs, `MINIMIZE`, `=`, `by`, late errors on a second decision; a plan whose one sort and cross-product unpivot carry only narrow columns and that has no group-back; tied items of several decisions sharing in proportion to their room. Scoped cases: several `by` groups at different prices, `when` in front of the clause, inside the sum and in both places, `by` with `when`, a NULL key, a group with no covered row, `by` with `>=` and `BETWEEN`. Error cases that must fail on both paths (a `when` keeping no row, a sum's own `when` keeping none of the clause's rows, an unbounded row outside the `when`, empty box, unreachable budget or fill, crossed limits, unbounded rows, including an uncapped improving row under `>=`, NULL and NaN values, late bad rows). Boundary cases against the solver only (a `BOOLEAN` column as a weight; an upper data limit of −infinity beside a row with no floor). Data-valued limits: a column, global and per group, an expression, an aggregate, global and with `by`, under `when`, a lower and an equality limit, a constant and a data clause mixed, two decisions with a column limit; errors for an equality limit that varies in a group and NULL and NaN limits; an equality to a data limit of 1e19, which is a number and not "no bound"; an equality that differs without a scope, and over the rows outside the sum's `when`, raises, and one that differs only among rows outside the `when` does not; a lower limit on a sum with its own `when` takes the maximum over the rows outside it too, and is infeasible when that maximum is out of reach; a data limit, global with one decision and per group with two, runs the plan of a constant limit. The plan has one sort and no solver operator; on the aggregate plan the sort carries only narrow columns (checked with twenty unused source columns), a stored table is read four times, and a scoped budget adds hash aggregates and joins. Objective `when` shapes: in front, inside the sum, in both places, on one part and on every part, a NULL `when`, one that keeps no row, under `>=` and `=`, beside a budget `when` and a `by` key, two decisions, an uncapped row whose value it drops inside and outside the budget, NaN, infinite and raising values on the rows it drops, and the NaN and unbounded errors on a row it keeps, word for word with the solver's. The number-range warning on the direct path: a price below the normal range warns, and prices in it and a value of 0 are silent. The row stage: one `direct_stage` CTE read four times when the copy fits, nested innermost under the source, bound and verdict CTEs, and four scans when it does not. Two rows of a group that fail one check in different ways raise the least message, whichever comes first. |
| `test_direct_rule_conformance.py` | R1's fixture: the checks every rule owes, and one near miss (an `INT` decision) that must decline under `require`, keep the solver plan, and answer under `auto` as under `off`. |
| `test_direct_user_facing.py` | R1's NULL, NaN, infinite and overflow errors name the right clause, and the infeasible and unbounded text match the solver's, compared in full. |
| C++ `[decidb]` | R1's applicability condition: each decline branch (among them budget clauses that cover different rows, budgets that are not one sum, and limits the solver reads as no bound), and the forms that apply (a column and an aggregate limit, a minimum, an exact fill, one sum compared twice, two decisions); R1 never applies where S1 or A1 does; the rule driver reaches R1, S1 and A1 each for their own shapes. |

## Performance

Measured on 2026-11-09 at `0ab6224c60`, on an 11-core Apple M3 Pro at a load of about 3.

**Against a hand-written plan.** `benchmark/decide/direct_solve/run.py --compare --sf 1 --queries r1 r1_global` runs the
direct plan beside a query written by hand for the same answer (`hand/<query>.sql`), with the same DOUBLE arithmetic, in
one process, best of 5, over the 800,000 rows of `partsupp`. CPU is every operator's time summed; latency is on all
threads; the planning DECIDE adds is apart. Both sides gave the same answer, to 2e-14 relative.

| Query | Direct CPU | Hand CPU | CPU ratio | Direct latency | Hand latency | DECIDE planning |
|---|---:|---:|---:|---:|---:|---:|
| `r1` | 648.1 ms | 335.8 ms | 1.93 | 99.7 ms | 60.4 ms | 1.19 ms |
| `r1_global` | 438.6 ms | 456.3 ms | 0.96 | 72.3 ms | 112.5 ms | 1.18 ms |

With one budget over every row (`r1_global`) the plan matches the hand plan. With a budget per group (`r1`) it takes
1.93 times its CPU. A group's checks (a budget no row can meet, an item that improves without limit) must raise even
when the query reads only some rows, under `LIMIT`, so the plan aggregates every row of each group and joins the verdict
back before it releases a row. The hand plan reads each row's answer from two windows and leaves the checks out, since
TPC-H's data cannot trip them (every box is finite and starts at 0). Reading the answer from window frames instead would
need a second plan shape for several decisions, and is not done for this gain (`../../framework/todo.md` § How to work
any task, Small gains).

**Against the solvers.** `run.py --sf 0.125 0.25 0.5 1 --queries r1 r1_global` (Gurobi) and the same with `--modes
direct highs`: a fresh `decidb` process per run, median of 3, load 3 to 6. At every size each solver's objective agreed
with direct solve's within 1e-6. A speedup divides the solver's median by direct's median from the same sweep. Times
include process start and database open, about 20 ms, which is most of the direct time at these sizes.

| Query | Rows | Direct | Direct, 1 thread | Direct peak | Gurobi | Gurobi peak | HiGHS | HiGHS peak | Speedup (Gurobi, HiGHS) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `r1` | 100,000 | 0.071 s | 0.069 s | 133 MiB | 0.156 s | 125 MiB | 0.109 s | 118 MiB | 2.2x, 1.6x |
| `r1` | 200,000 | 0.079 s | 0.114 s | 173 MiB | 0.310 s | 203 MiB | 0.216 s | 206 MiB | 3.9x, 2.6x |
| `r1` | 400,000 | 0.092 s | 0.198 s | 229 MiB | 0.671 s | 422 MiB | 0.411 s | 377 MiB | 7.3x, 4.0x |
| `r1` | 800,000 | 0.120 s | 0.375 s | 345 MiB | 1.386 s | 855 MiB | 0.794 s | 764 MiB | 11.5x, 6.5x |
| `r1_global` | 100,000 | 0.042 s | 0.057 s | 70 MiB | 0.140 s | 130 MiB | 0.111 s | 98 MiB | 3.3x, 2.9x |
| `r1_global` | 200,000 | 0.048 s | 0.092 s | 94 MiB | 0.259 s | 216 MiB | 0.247 s | 174 MiB | 5.5x, 4.5x |
| `r1_global` | 400,000 | 0.062 s | 0.164 s | 149 MiB | 0.576 s | 390 MiB | 0.525 s | 319 MiB | 9.3x, 7.8x |
| `r1_global` | 800,000 | 0.092 s | 0.308 s | 254 MiB | 1.132 s | 744 MiB | 1.047 s | 623 MiB | 12.3x, 10.4x |

Direct solve is ahead of both solvers at every size, 11.5 to 12.3 times Gurobi and 6.5 to 10.4 times HiGHS at 800,000
rows, in a third to two fifths of Gurobi's peak memory. It runs on all threads: 0.12 s against 0.38 s on one for `r1` at
800,000 rows.