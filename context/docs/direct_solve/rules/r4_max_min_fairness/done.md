# R4 (weighted max-min fair allocation) — what works today

Verified on 2026-11-13 at `f380987f65`: `test/decide/run_direct_matrix.sh`, each of its four suites (auto, off,
serializer, highs) 7,283 passed; C++ `build/release/test/unittest "[decidb]"` 11,171 assertions in 107 cases. Code:
`src/optimizer/decide/direct/r4_max_min_fairness_rule.cpp`. Class definition: [definition.md](definition.md).

## What it admits

One `REAL` decision, on each row or shared by the rows of a key (`per K:`), with:

- **One objective.** `MAXIMIZE MIN(c*x)`: one plain `MIN` over one linear term with a coefficient read from the row and
  no constant factor, read on the canonical form, so `MIN(x / demand)` is `c = 1/demand`. A constant beside the `MIN`,
  or one written inside it that canonicalization moves out, moves no answer. A `when` on the objective or on the part
  keeps the rows the `MIN` reads.
- **One budget.** `SUM(w*x) <= B`, the only aggregate clause. The weight `w` is a constant or read from the row, of any
  sign. The limit `B` is a constant or read from the data. The budget may be per `by` group and under a `when`.
- **A box `[0, cap]`.** The lower end is 0 (the default or written). Every other clause bounds one row's decision from
  above, `a*x <= r` with a constant `a > 0`; `r` is a constant, read from the row, or absent.
- **A shared decision** (`per K:`) when every budget group lies within the key and the budget has no `when`, so all the
  rows of a key fall in one group.

## What it does not admit (falls back to the solver)

A decision of the whole query, two decisions, an `INT` or `BOOL` decision, a lower bound other than 0, an upper bound
with a coefficient read from the data (its sign decides the side), a second aggregate clause, a budget from below or
an equality, a budget `per K:`, a square in the budget, a minimized `MIN`, a `MIN` with a constant factor, two objective
parts, and, for a shared decision, a budget with a `when` or groups finer than or across the key. The C++ decline
cases (`test_decidb_direct_decline.cpp`, `R4DeclineShapes`) name each.

## The answer

Every row the `MIN` reads must reach a common level `s`: `c*x >= s`, so `x >= s/c`. Each row takes the end of its range
that uses the least budget, by the sign of its weight:

- **`w >= 0`:** the low end, `s/c` for a row the `MIN` reads with `c > 0`, and 0 for any other row. A weight of 0
  costs nothing, so it needs no case.
- **`w < 0`:** its capacity, which frees `-w*cap` of the budget. A row with no capacity (NULL, or 1e20 or more) frees
  as much as the other rows need: it takes `max(lo, D/W)`, where `lo` is its own low end, `D` is what its group's other
  rows need beyond `B` plus the capped rows' freed budget, and `W` is the sum of `-w` over the uncapped rows.

Per budget group `g`, with `N` the sum of `w/c` over the rows the `MIN` reads with `w > 0` and `c > 0`, `F` the freed
budget of the capped negative-weight rows, and `U` the least `cap*c`, the level is `s_g = min((B+F)/N, U)`, or `U`
alone when an uncapped negative-weight row makes the budget unlimited. A group with a NULL limit, or none of whose rows
the budget keeps, is no budget; the rows outside every budget form one more group whose level is their `U`.

The objective is the least `c*x` over all rows, so the optimal value is `s* = min_g s_g`. Each group takes its own
level, which is optimal and uses its own budget; a group whose level is infinite takes `s*`, so its rows stay bounded.
Several assignments reach `s*`: the solver may return a different one, and the tests compare the objective value and
feasibility.

**Like the solver.** A demand of 0 makes `c` infinite and raises the solver's NaN-or-infinite message for `1 / demand`.
Any `c <= 0` on a row the `MIN` reads makes the level 0, and every amount 0. A row the `MIN` does not read (a NULL
demand, a row a `when` drops) takes its cheap end. A `MIN` that reads no row gives every row 0, as the solver does. A
NULL weight counts as 0. A NULL capacity is no bound. A negative capacity raises infeasible; so does a group whose
`B + F` is below 0 with no uncapped negative-weight row. An infinite `s*` raises unbounded. An empty source returns no
rows, even under a negative budget. For a shared decision, a row whose key puts it in no class has no decision: nothing
is read on it and its amount is NULL, while a limit read from it still counts in its group's tightest limit, as on the
solver path.

## How it works

The plan has N1's shape: two reads of the source. The first computes each row's values and checks, then one hash
aggregate on the budget's group key gives the sums `N`, `F`, `W` and the least `U` per group, with one row for the rows
outside every budget; each group's level, and `s*` over the groups, follow. The second read joins each row to its
group's row and computes the row's amount. The join compares the group key with IS NOT DISTINCT FROM, so the rows
outside every budget find their group's row through the same join. Every check stands below the hash aggregate, which
holds every row before it gives one, so a bad value on the last row raises even under `LIMIT 1`. A shared decision
gives every row of its key the key's weight sum, least capacity and least coefficient through a window by key in each
read, and the first row of a key stands for it in its group's sums. The stage-by-stage layout is the comment above
`PlanBuilder` in `src/optimizer/decide/direct/r4_max_min_fairness_rule.cpp`.

Recognition reads only the IR. A weight that folds to a constant is a literal the plan builds with; a weight read from
the data has its sign handled inside the same aggregate pass, so no plan-time read of the data is needed.

## Verified by

- `test/decide/tests/test_direct_r4_max_min_fairness.py`: the oracle file, which checks each DeciDB assignment's
  feasibility and objective against an independent Gurobi model, on `off` and `require`, with an error table and the
  no-row and empty-source cases pinned on both paths.
- The R4 `RuleFixture` in `_direct_rule_fixtures.py`, run by `test_direct_rule_conformance.py`, with `late_errors`
  over many groups and the near misses.
- `test/common/test_decidb_direct_decline.cpp`: `R4DeclineShapes` and R4's row in `EveryRuleWithItsShapes`.
