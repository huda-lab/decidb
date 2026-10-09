# A1 (independent bounded decisions) — what works today

Verified on 2026-10-06 on top of `db4fc9d4f3` (working tree, not yet committed):
`make decide-test` 2,250 passed and 6 expected failures, plus the 7 existing tests listed in `todo.md` (A1-01);
`DECIDB_TEST_DIRECT_SOLVE=off make decide-test` 2,257 passed, 6 expected failures, none failing;
`DECIDB_VERIFY_SERIALIZER=1 make decide-test` the same as `auto`;
`DECIDB_FORCE_SOLVER=highs make decide-test` 2,256 passed, 6 expected failures, 1 unrelated failure that needs Gurobi;
`build/release/test/unittest "[decidb]"` 1,227 assertions in 39 cases.
Code: `src/optimizer/decide/direct/a1_rule.cpp`. Class definition: [definition.md](definition.md).

## What it admits

One or more row-scoped `BOOL`, `INT` or `REAL` decisions, each solved on its own, with:

- **Bounds, per row.** Any comparison (`=`, `<`, `<=`, `>`, `>=`, `BETWEEN`, either side) of a decision against a constant,
  a numeric or Boolean source column (a Boolean is read as 0 or 1, for `BOOL` and `INT`), or a deterministic expression
  over source columns. The decision may carry a coefficient and an offset (`2*x + 1 <= cap`, `-x >= -cap`, `w*x <= cap`);
  a row-valued coefficient picks the side by its sign, and a zero coefficient leaves the row alone or infeasible. Each
  clause may carry its own `WHEN`. The value expressions may raise at run time (an overflowing cast, a division by
  zero): the plan evaluates them on every row, as the solver does. An equality is an at-least clause and an at-most
  clause; the second is `A1Clause::Copy()` of the first with its comparison changed, so no field can differ.
- **Default box.** `BOOL` is [0, 1]; `INT` and `REAL` are [0, ∞). A bare unconditional constant lower bound (`x >= -3`)
  replaces the default 0 on the solver, so it does here. Every other lower bound (a column, a `WHEN`, a coefficient or
  offset on `x`, an arithmetic constant such as `0 - 3`) only intersects with it. The solver and direct solve ask one
  shared predicate which clauses are folded into the variable (`DecideBoundIsAbsorbed`, surfaced as
  `DirectConstraintFact::variable_bound`), so they cannot disagree. A bound of magnitude 1e20 or more is no bound, as on
  the solver; an infinite data value is no bound on that side.
- **Whole-number rounding.** `INT` and `BOOL` bounds round inward, strict bounds included, and the rounding is tested
  in `DOUBLE` arithmetic against the clause as written, so `0.1*x <= 0.3` does not lose an integer to a quotient that
  rounds the wrong way. A strict bound against a fraction (`x > 0.5`, `x < 2.5`, no `WHEN`) rounds to the first whole
  value inside it (1 and 2). The solver and A1 take that shift from one function (`DecideStrictBound`), so they agree;
  on a `BOOL`, A1 and the solver now both answer `BOOL x > 0.5` with `x = 1`. The six cases are in `SOLVER_OFF_BY_ONE`
  (`test_direct_a1.py`), checked against the oracle on both paths. S1 admits the same bounds on a `BOOL`
  (`x > 0.5`, `x < 1.5`) with the same shift.
- **Objective.** `MINIMIZE`, `MAXIMIZE`, or none (the lowest allowed value). Signed `SUM` parts, each with an optional
  constant factor or divisor and an optional `WHEN`, plus an optional objective-level `WHEN`. Terms: a row-valued
  coefficient times a decision, a per-row constant, and at most one square `k*(w*x + t)^2`, where `k` is a nonzero
  constant and `w` and `t` are row-valued (`w` may be zero or negative). `POWER(x - t, 2)`, `(x - t)*(x - t)`,
  `x*x - 2*t*x`, `norm(e, 2)` and a constant factor inside or outside the `SUM` are all this shape.
- **Per row, the optimum** is an endpoint or the clamped turning point. For whole numbers the turning point is replaced
  by the first integer whose next step no longer improves, so a tie goes to the smaller value (as on the solver).
  Maximizing a convex square and minimizing a concave one compare the two endpoints.

It raises an error wherever the solver does: NULL, NaN or infinite coefficients and NULL or NaN bounds (including on
rows a `WHEN` excludes), an empty box on any row (infeasible), an improving direction with no finite limit on any row
(unbounded), an `INT` answer beyond 2^53, an objective `WHEN` that matches no row of a non-empty source, and any
expression that raises. Every input row is read before one is released, so a late bad row raises under `LIMIT 1`,
`COUNT(*)` or a filter; only `LIMIT 0` skips it. An empty source returns no rows.

## What it does not admit (falls back to the solver)

A miss never changes the answer: the query runs on the solver and `require` names the clause.

| Shape | Why |
|---|---|
| An aggregate constraint (`SUM(x) <= 5`, `norm(x, 1) <= 5`) | Rows are coupled. Count bounds on one `BOOL` are S1. |
| `x <> k`, `x IN (...)` | Not an interval. The solver reads them as a split or discrete domain. |
| Two decisions in one constraint or one objective term (`x + y <= 3`, `x*y`), `ABS`, a squared constraint | Decisions are coupled. |
| Objective `PER`; `AVG`, `MIN`, `MAX`, `norm` of order 1 or ∞ | Not a sum of one term per row. |
| Entity-scoped (`T.x`) or scalar decisions | Not one decision per row. |
| `random()`, a correlated subquery, a parameter, in a bound, a coefficient or a `WHEN` | Not a deterministic source value. A scalar subquery that does not depend on the row is read as a source column and is admitted. |
| A second squared term, a data weight outside a square (`w*POWER(...)`), `w*x*x` | The solver rejects these itself, with the same message, on both paths. |
| Strict bounds on a `REAL` | The binder rejects them before any rule runs. |
| A bound that contradicts the variable's built-in domain (`x <= -1` on a non-negative decision, `BOOL x >= 2`) | The solver rejects it before it reads a row, even on an empty source, so the solver reports it. |

## Where it differs from the solver

- **Values within the solver's tolerance of an integer** (`INT x <= 1.9999999`). Direct solve reads every bound exactly,
  as S1 decided; the solver accepts a violation of about 1e-6 and returns 2. The tests and the fuzz use values that are
  exact in a double, so they never sit in that band.
- **Error wording.** An empty box on a row reads `DECIDE optimization is infeasible. Prefix the query with DIAGNOSE
  ...` here. A bound that contradicts the built-in domain is left to the solver (see above), so its own message appears
  and quotes the clause as typed (`x < 0 cannot hold because x is non-negative`). A NULL value names its source column.
  The "at row N" suffix is left out.
- **Ties.** The smaller value, which matches the solver in every case checked, but nothing promises it.
- **A decision that appears in no objective term** takes the lowest value its box allows.

## How it works

One projection per stage, with no sort, no window and no join:

1. raw values (every coefficient, bound, mask), evaluated unconditionally on every row;
2. each clause's lower and upper bound on its decision, each decision's objective coefficients `(a, b)`, and the check on
   every raw value;
3. each decision's box;
4. each decision's answer and whether its box is empty;
5. the rows are held until the check has run on all of them (`DirectValidationBarrier`), then the typed answers are
   projected: `INTEGER` for `BOOL`, `BIGINT` for `INT`, `DOUBLE` for `REAL`.

A problem made only of constants, with no `WHEN`, fails the same way on every row, so it needs no barrier: the check is
a projected validation column and the plan streams. Unreferenced source columns never reach the barrier; a column the
parent reads does, so reading a wide column back costs memory in proportion to the input.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_a1.py` (268 tests) | The answers. 138 cases run on the independent oracle (`oracle_solver`, built from the raw rows and the per-row bounds each case states in Python), on the solver path (`off`) and on the direct path (`require`): constant boxes, the default-lower rule in every spelling that does and does not replace it, row-valued bounds with and without `WHEN`, coefficient forms (positive, negative, zero), `INT` rounding and strict bounds, `BOOL` pins and Boolean-column bounds, every objective spelling, objective `WHEN`, infeasible and unbounded problems, ties, and three decisions of different types. 28 parent-query contexts. 39 error cases that must fail on both paths. 23 boundary cases against the solver path only (infinities, the 1e20 and 2^53 limits, every numeric source type). 6 random-row tests against exact `Fraction` arithmetic. The constant-problem tests (streaming plan, errors under `LIMIT 1`, `COUNT(*)` and a filter, empty source), a nested `DECIDE`, and duplicate rows |
| `test_direct_rule_contract.py` | The checks every rule owes, with A1's fixture: schema and rows, all-rows read for 10 kinds of late bad row, serializer, prepared plans, `EXPLAIN` and profiling, `off`, forced HiGHS, `DIAGNOSE`, and 17 near misses that must name their reason and answer under `auto` as under `off` |
| `test_direct_fuzz.py` | 8 seeds of 30 random A1 queries: direct against solver over domains, bounds, objectives, `WHEN`, `NULL` and infinite values |
| `test_direct_user_facing.py` | A 200-byte unused payload column stays out of the scan and the barrier; a computed column that can raise is not pruned |
| Five tests pinned to the solver path | `test_canonicalize_idempotence.py` (three cases) and `test_implied_bounds.py` (two cases) inspect the solver's model dump. Direct solve builds no model, so they run with `DECIDB_TEST_DIRECT_SOLVE=off` on purpose. |
| C++ `[decidb]` | `variable_bound` over 19 spellings, the facts for every objective spelling, A1's proof and each decline reason, S1 and A1 never both proving, and the coordinator choosing the right real rule for each shape |

## Performance

Measured 2026-10-06 on this machine, with `benchmark/decide/profile_direct_a1.py`. Each run is a fresh `decidb` process
that opens the stored database; peak memory is the process's own. Direct 2 to 3 runs, solver (Gurobi) 1 run. The result is
one aggregate row, so the time is the plan, not the transfer.

| Shape, 5,000,000 rows | Direct | Direct peak | Solver | Solver peak | Speedup |
|---|---:|---:|---:|---:|---:|
| `REAL`, quadratic, per-row bounds | 0.31 s | 146 MiB | 2.88 s | 5.6 GiB | 9.2x |
| the same with a `WHEN` on the objective | 0.49 s | 270 MiB | 2.62 s | 5.0 GiB | 5.3x |
| `INT`, linear, per-row bounds | 0.39 s | 137 MiB | 2.69 s | 4.4 GiB | 6.9x |
| `BOOL`, linear, bounds only | 0.22 s | 135 MiB | 0.97 s | 1.0 GiB | 4.4x |
| constants only (no barrier) | 0.03 s | 34 MiB | 0.80 s | 1.4 GiB | 29x |

At 1,000,000 rows the same shapes are 4.5 to 8x faster and use 31 to 73 MiB against 0.2 to 1.7 GiB. At 100,000 rows both
paths finish in under 0.1 s.

**Weak spots.** A parent that reads a wide column back makes the barrier hold it: with a 512-byte payload read at
1,000,000 rows direct peaks at 1.06 GiB (0.26 s against 0.59 s, 2.3x) instead of 47 MiB. The speedup is smaller than
S1's because the solver is already cheap on a bound-only problem. 5,000,000 wide rows were not run (about 3 GiB of
disk and several GiB of memory).

**Reproduce** (after `make release`; the sweeps above took under a minute in total, building the databases included):

```sh
python3 benchmark/decide/profile_direct_a1.py --rows 100000 1000000 --db-dir /tmp/a1db
python3 benchmark/decide/profile_direct_a1.py --rows 5000000 --repeats 3 --db-dir /tmp/a1db \
  --shapes real_quadratic real_quadratic_when int_linear bool_bounds constant_problem count_only
```

The runner pins `require` for direct and `off` for the solver, and runs the solver at a size only if its previous size
took at most `--solver-budget` seconds. The 5,000,000-row database needs only the narrow table; build it by hand to skip
the wide one.

## Settled follow-ups (2026-10-09)

- **Strict bound against a fraction.** The solver's bound absorption and A1 share one function for the strict shift,
  `DecideStrictBound` (`decide_bound_absorption.{hpp,cpp}`): `floor(k) + 1` for `>` and `ceil(k) - 1` for `<`. The solver
  once moved the bound by a whole unit before rounding (`INT x > 0.5` gave 2, `INT x < 2.5` gave 1, and `BOOL x > 0.5`
  raised an error). All six cases in `SOLVER_OFF_BY_ONE` now agree with the oracle on the direct and the solver path.
  A1 has no special case for `BOOL`.
- **Domain rejection wording.** The solver's check of a bound against the built-in domain quotes the clause as typed
  (`x < 0`, not the shifted `x <= -1`; `BOOL x > 1.5`, not `x >= 2`). `test_error_infeasible.py` asserts the original
  patterns again (`x <= -1 cannot hold ... non-negative`, `x >= 2 cannot hold ... BOOLEAN`) and has new cases for the
  strict spellings.
