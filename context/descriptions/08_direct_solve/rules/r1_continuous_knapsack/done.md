# R1 (continuous knapsack) — what works today

Verified on 2026-10-22 on top of `aa9ee8fe6b`: `make decide-test`, `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`
and `DECIDB_VERIFY_SERIALIZER=1 make decide-test` each 3,089 passed (no failures, no expected failures);
`DECIDB_FORCE_SOLVER=highs make decide-test` 3,088 passed and one failure that needs Gurobi
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
- **Error wording.** Infeasible, unbounded, NULL and NaN errors read as the solver's; `test_direct_user_facing.py`
  compares them in full.

## How it works

`definition.md` § Relational construction. The scope projection (`DirectProjectScope`) marks each row eligible when the
budget's `WHEN` holds and no `PER` key is NULL, and the empty-aggregate guard (`DirectGuardEmptyAggregate`) runs on it.
A data-valued limit is validated and reduced per group by `DirectValidateBounds` before the
stages below, and each row reads its group's `L` and `U` as columns. Every window and the one sort are partitioned by
eligibility and the `PER` keys, so each group gets its own verdicts,
target and price; an ineligible row answers as a zero-weight row. Each stage is a projection
or a window over every column so far; the plan has one sort.

With several decisions, the plan first unpivots the rows into items, as DuckDB's own `UNPIVOT` does: it numbers the
rows with `ROW_NUMBER() OVER ()` (no order, so no sort) and unnests each into one item per decision, carrying that
decision's weight, value and box and every source column. The stages below run on items. After the breaker a hash
aggregate on the row number groups the items back: the source columns by `any_value`, each decision's answer by
`any_value` of the answer where the item is that decision. Late materialization does not apply to this plan, so a
wide source column is carried once per decision through the sort. One decision keeps the plan without the unnest and
the aggregate.

1. Raw values (every coefficient, bound, mask), evaluated on every row; each clause's bounds, the value `v` (negated
   under `MINIMIZE`, zero with no objective) and the check on every raw value; the box `[l, u]`.
2. Per item: whether the box is empty, the anchor, the break-even price `t = v / w` (items with `w = 0` sort last and
   take their best end), and the finite room up and down. An infinite room is a flag, counted beside the finite sums,
   so no window adds infinity to minus infinity; a NaN would sort above every price and pick the wrong one.
3. Whole-input windows: the anchors' spend, the finite room down and the count of infinite ones, the extreme prices of
   the items with infinite room, and the zero-price spend.
4. The one sort, by `t` highest first, ties in any order: running sums of the finite rooms and of the infinite counts.
5. The verdicts (infeasible before unbounded). When the zero-price spend is above `U`, the price is the highest `t`
   among the items whose spend, with the items before them at their high end, reaches `U`. When it is below `L`, the
   price is the lowest `t` among the items whose spend, with them and the items after them at their low end, does not
   pass `L`. Both tests are monotone along the one sort, because rounded sums do not land exactly on a limit.
6. Whole-input windows of the rooms above, below and at the price; each item takes its high end, its low end or its
   share of what the budget leaves the items at the price, from its anchor, held to its box. Infinite rooms at the price
   share the remainder evenly.
7. The rows are held until the check has run on all of them (`DirectValidationBreaker`), then `x` is projected as
   `DOUBLE`.

An empty source returns no rows, whatever the limits are, as on the solver.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_r1_continuous_knapsack.py` | The answers on the independent oracle (`oracle_solver`), the solver path (`off`) and the direct path (`require`): weights of each sign and zero, positive lower bounds, per-row and constant bounds, a lone free row, an improving pair, no objective, `MINIMIZE`, a budget that does not bind, ties; each budget direction binding and not binding, an exact fill reached from above and from below and at a negative price, `BETWEEN` binding at either side, and the minimum-cost cover. Parent-query contexts, random rows, an empty source, a nested `DECIDE`, a parent filter and `LIMIT`, duplicate rows. Several decisions: two with shrinking and with growing piece values, three, a decision outside the budget, mixed signs, `MINIMIZE`, `=`, `PER`, parent contexts, late errors on a second decision, and a plan with one sort, one unnest and one aggregate. Scoped cases: several `PER` groups at different prices, `WHEN` in both spellings, `PER` with `WHEN`, a NULL key, a group with no covered row, `PER` with `>=` and `BETWEEN`; scopes that differ stay on the solver. Error cases that must fail on both paths (a `WHEN` covering no row, an unbounded row outside the `WHEN`, empty box, unreachable budget or fill, crossed limits, unbounded rows, including an uncapped improving row under `>=`, NULL and NaN values, late bad rows). Boundary cases against the solver only (1e20 bounds, infinities). Data-valued limits: a column, global and per group, an expression, an aggregate, global and with `PER`, under `WHEN`, a lower and an equality limit, a constant and a data clause mixed, two decisions with a column limit; errors for an equality limit that varies in a group and NULL and NaN limits; boundary cases for 1e20, 1e25 and infinite data limits. Constant limits the solver reads by the rows stay on the solver. The plan has one sort and no solver operator. |
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

**Where the time goes.** At 1,000,000 rows (`<=`, positive weights, 1.52 s) the sorted window takes 0.81 s and the
three whole-input windows about 0.1 s each; the projections are under 0.08 s each. A window with no `PARTITION BY` is
one partition, and every window carries every source column, so the plan holds the whole input several times. That is
why `PER` shapes, whose windows split into many partitions, are the only clear wins against Gurobi, and why peak
memory is the solver's size rather than A1's. Two decisions add the unnest and the hash aggregate that groups items
back, and late materialization does not apply to that plan.

**Reproduce** (after `make release`; the sweep took about 20 minutes, building the databases included):

```sh
python3 benchmark/decide/profile_direct_r1.py --rows 1000000 5000000 --repeats 3 --solver-budget 60 --db-dir build/r1db
```

