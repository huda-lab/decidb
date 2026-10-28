# A1 (separable box-constrained problem) — what works today

Verified on 2026-10-27 at `a14b1f8129` or later, with the strict xfails on the direct path of pending cases: `make decide-test`,
`DECIDB_TEST_DIRECT_SOLVE=off make decide-test` and `DECIDB_VERIFY_SERIALIZER=1 make decide-test` each 4,521 passed
and 335 strict expected failures (the direct path of keyed and folded-affine cases, other direct-solve follow-ups
and the deferred bound reader); `DECIDB_FORCE_SOLVER=highs
make decide-test` 4,519 passed and two failures, `test_norm_combined_l1_l2_objective`, which needs Gurobi, and
`test_entity_scoped_mixed_when_per`, which HiGHS cannot close at a gap of 0 in time (`../../../issues/bugs.md`); C++
`build/release/test/unittest "[decidb]"` 2,714 assertions in 65 cases. `test/decide/run_direct_matrix.sh` renews these numbers in one command (it prints one line per check).
Code: `src/optimizer/decide/direct/a1_separable_box_rule.cpp`; its per-row bounds come from the shared reader in
`direct_bound_reader.cpp`. Class definition: [definition.md](definition.md).

## What it admits

One or more row-scoped `BOOL`, `INT` or `REAL` decisions, each solved on its own, with:

- **Bounds, per row.** Any comparison (`=`, `<`, `<=`, `>`, `>=`, `BETWEEN`, either side) of a decision against a constant,
  a numeric or Boolean source column (a Boolean is read as 0 or 1, for `BOOL` and `INT`), or a deterministic expression
  over source columns. The decision may carry a coefficient and an offset (`2*x + 1 <= cap`, `-x >= -cap`, `w*x <= cap`);
  a row-valued coefficient picks the side by its sign, and a zero coefficient leaves the row alone or infeasible. Each
  clause may carry its own `when`. The value expressions may raise at run time (an overflowing cast, a division by
  zero): the plan evaluates them on every row the clause's `when` keeps, as the solver does, and computes nothing on a
  row the `when` drops (syntax reference §6.3). Each clause reads as the lower and upper end of the values it admits,
  the two expressions the solver path's classifier builds (`DecideReadOneDecisionBound`), so an equality is one clause
  with both ends set.
- **Default box.** `BOOL` is [0, 1]; `INT` and `REAL` are [0, ∞). An unconditional constant lower bound replaces the
  default 0 on the solver, so it does here, in any spelling the classifier reads as a bound on one decision (`x >= -3`,
  `2*x >= -6`, `x + 1 >= -3`, `x >= 0 - 3`, D41). A lower bound from a column or under a `when` only intersects with the
  default. The solver and direct solve read which clauses are folded into the variable from the same classifier
  (`DirectIRConstraint::variable_bound`), and combine the folded values with one function (`DecideFoldSingletonRow`), so
  they cannot disagree. A bound of magnitude 1e20 or more is no bound, as on the solver, including a written -1e30 and a
  literal `-inf` floor. An infinite data value is no bound in the open direction (`x <= cap` with `cap = +inf`) and
  admits nothing in the other (`x >= cap`), which empties the row's box (D65).
- **Whole-number rounding.** An `INT` or `BOOL` decision admits the whole numbers the written comparison admits as
  DuckDB evaluates it (D64), strict bounds included: `0.1*x <= 0.3` admits 3 (the DECIMAL product is exactly 0.3), and
  `x > 0.5`, `x < 2.5` admit 1 and 2 at their ends. The six strict cases are in `SOLVER_OFF_BY_ONE`
  (`test_direct_a1_separable_box.py`), checked against the oracle on both paths; `test_one_decision_bound.py` checks the
  DECIMAL and DOUBLE spellings against DuckDB on both paths and both backends. S1 reads the same interval on a `BOOL`.
- **Objective.** `MINIMIZE`, `MAXIMIZE`, or none (the lowest allowed value). Signed `SUM` parts, each with an optional
  constant factor or divisor and an optional `when`, plus an optional objective-level `when`. A part's values are
  computed only on the rows its `when` and the objective's keep. Terms: a row-valued
  coefficient times a decision, a per-row constant, and at most one square `k*(w*x + t)^2`, where `k` is a nonzero
  constant and `w` and `t` are row-valued (`w` may be zero or negative). `POWER(x - t, 2)`, `(x - t)*(x - t)`,
  `x*x - 2*t*x`, `norm(e, 2)` and a constant factor inside or outside the `SUM` are all this shape.
- **Per row, the optimum** is an endpoint or the clamped turning point. For whole numbers the turning point is replaced
  by the first integer whose next step no longer improves, so a tie goes to the smaller value (as on the solver).
  Maximizing a convex square and minimizing a concave one compare the two endpoints.

It raises an error wherever the solver does: NULL, NaN or infinite coefficients and NULL or NaN bounds on a row that
reads them, an empty box on any row (infeasible), an improving direction with no finite limit on any row (unbounded), an
`INT` answer beyond 2^53 after rounding (with the solver's message word for word, naming the variable and its rounded
value), a `when` that keeps no row of a non-empty source (D4, raised by the plan's row checks), and any expression that
raises on a row that computes it. Every input row is read before one is released, so a late bad row raises under `LIMIT 1`,
`COUNT(*)` or a filter; only `LIMIT 0` skips it. An empty source returns no rows.

## What it does not admit (falls back to the solver)

A query A1 does not apply to keeps its answer: it runs on the solver, and `require` raises one fixed error.

| Shape | Why |
|---|---|
| An aggregate constraint (`SUM(x) <= 5`, `norm(x, 1) <= 5`) | Rows are coupled. Count bounds on one `BOOL` are S1. |
| `x <> k`, `x IN (...)` | Not an interval. The solver reads them as a split or discrete domain. |
| Two decisions in one constraint or one objective term (`x + y <= 3`, `x*y`), `ABS`, a squared constraint | Decisions are coupled. |
| An objective with a `per` or a two-level reducer; `AVG`, `MIN`, `MAX`, `norm` of order 1 or ∞ | Not a sum of one term per row. |
| Entity-scoped (`T.x`) or scalar decisions | Not one decision per row. |
| `random()`, a correlated subquery, a parameter, in a bound, a coefficient or a `when` | Not a deterministic source value. A scalar subquery that does not depend on the row is read as a source column and is admitted. |
| A second squared term, a data weight outside a square (`w*POWER(...)`), `w*x*x` | The solver rejects these itself, with the same message, on both paths. |
| Strict bounds on a `REAL` | The binder rejects them before any rule runs. |
| A bound that contradicts the variable's built-in domain (`x <= -1` on a non-negative decision, `BOOL x >= 2`) | The solver rejects it before it reads a row, even on an empty source, so the solver reports it. |

## Where it differs from the solver

- **Values within the solver's tolerance of an integer** (`INT x <= 1.9999999`). Direct solve reads every bound exactly,
  as S1 decided; the solver accepts a violation of about 1e-6 and returns 2. The tests use values that are
  exact in a double, so they never sit in that band.
- **Error wording.** An empty box on a row reads `DECIDE optimization is infeasible. Prefix the query with DIAGNOSE
  ...` here. A bound that contradicts the built-in domain is left to the solver (see above), so its own message appears:
  for a non-negative decision it quotes the clause as typed (`x < 0 cannot hold because x is non-negative`), and for a
  `BOOL` too (`x >= 2 cannot hold for a BOOL variable.`). A NULL coefficient, target, weight or bound names the columns that are NULL
  on the failing row, in the solver's text, and a NaN value reads as the solver's message; both are built by the
  functions the solver calls (`decide_value_messages.hpp`). The "at row N" suffix is left out.
- **Ties.** The smaller value, which matches the solver in every case checked, but nothing promises it.
- **A decision that appears in no objective term** takes the lowest value its box allows.

## How it works

One projection per stage, with no sort, no window and no join:

1. raw values (every coefficient, bound, mask), each evaluated on the rows its `when` keeps and NULL on the others
   (`DirectOnKeptRows`), with its checks masked the same way (`DirectCheckOnKeptRows`);
2. each clause's lower and upper bound on its decision, each decision's objective coefficients `(a, b)`, and the check on
   every raw value;
3. each decision's box;
4. each decision's answer and whether its box is empty;
5. the rows are held until the check has run on all of them (`DirectValidationBreaker`), then the typed answers are
   projected: `INTEGER` for `BOOL`, `BIGINT` for `INT`, `DOUBLE` for `REAL`.

A problem made only of constants, with no `when`, fails the same way on every row, so it needs no breaker: the check is
a projected validation column and the plan is pipelined. Unreferenced source columns never reach the breaker; a column
the parent reads does, so reading a wide column back costs memory in proportion to the input.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_a1_separable_box.py` (231 tests) | The answers. 123 cases run on the independent oracle (`oracle_solver`, built from the raw rows and the per-row bounds each case states in Python), on the solver path (`off`) and on the direct path (`require`): constant boxes, the default-lower rule in every spelling that does and does not replace it, row-valued bounds with and without `when`, coefficient forms (positive, negative, zero), `INT` rounding and strict bounds, `BOOL` pins and Boolean-column bounds, every objective spelling, objective `when`, infeasible and unbounded problems, ties, and three decisions of different types. 28 parent-query contexts. 13 error cases that must fail on both paths. 21 boundary cases against the solver path only (infinities, the 1e20 and 2^53 limits, every numeric source type). 12 cases on how bounds combine, on both paths: a written lower bound of -1e30 or -1e31 is no bound for `INT` and `REAL`, a second lower bound beside it still applies, and no written lower bound leaves the floor at 0. 6 random-row tests against exact `Fraction` arithmetic. The constant-problem tests (streaming plan, errors under `LIMIT 1`, `COUNT(*)` and a filter, empty source), a nested `DECIDE`, and duplicate rows The affine spellings of a folded bound run on both paths. |
| `test_direct_rule_conformance.py` | The checks every rule owes, with A1's fixture: schema and rows, all-rows read for 10 kinds of late bad row, serializer, prepared plans, `EXPLAIN` and profiling, `off`, forced HiGHS, `DIAGNOSE`, and 17 near misses that must decline under `require` and answer under `auto` as under `off`, and a plain form that applies |
| `test_direct_user_facing.py` | A 200-byte unused payload column stays out of the scan and the breaker; a computed column that can raise is not pruned; a NaN in a constraint coefficient says "constraint coefficient" and in the objective "objective coefficient"; every NULL and NaN error reads as the solver's, compared in full |
| `test_error_infeasible.py` | The solver's rejection of a bound against the built-in domain quotes the clause as typed for a non-negative decision (`x <= -1 cannot hold ... non-negative`, strict `x < 0`); for a `BOOL` (`x >= 2`, `x > 1`, `x > 1.5`) it is the usual infeasible error |
| Five tests pinned to the solver path | `test_canonicalize_idempotence.py` (three cases) and `test_implied_bounds.py` (two cases) inspect the solver's model dump. Direct solve builds no model, so they run with `DECIDB_TEST_DIRECT_SOLVE=off` on purpose. |
| C++ `[decidb]` | `variable_bound` over 19 spellings, the IR for every objective spelling, A1's applicability condition and its declines, S1 and A1 never both applying, and the rule driver choosing the right real rule for each shape |

## Performance

Measured 2026-10-13 at `7a480a1a71` on this machine, with `benchmark/decide/direct_solve/archive/profile_direct_a1.py`. Each run is a fresh
`decidb` process that opens the stored database; peak memory is the process's own. Direct 3 runs, solver (Gurobi) 1 run.
The result is one aggregate row, so the time is the plan, not the transfer.

| Shape, 5,000,000 rows | Direct | Direct peak | Solver | Solver peak | Speedup |
|---|---:|---:|---:|---:|---:|
| `REAL`, quadratic, per-row bounds | 0.32 s | 146 MiB | 3.22 s | 5.5 GiB | 10x |
| the same, parent reads only `COUNT(*)` | 0.29 s | 106 MiB | 3.00 s | 5.5 GiB | 10x |
| the same with a `WHEN` on the objective | 0.51 s | 271 MiB | 2.73 s | 4.9 GiB | 5.4x |
| `INT`, linear, per-row bounds | 0.40 s | 137 MiB | 2.75 s | 4.3 GiB | 6.9x |
| `BOOL`, linear, bounds only | 0.23 s | 136 MiB | 0.99 s | 1.0 GiB | 4.3x |
| constants only (no breaker) | 0.03 s | 34 MiB | 0.82 s | 1.4 GiB | 29x |

At 1,000,000 rows the same shapes are 3.5 to 8.3x faster and use 31 to 73 MiB against 0.2 to 1.1 GiB. At 100,000 rows
both paths finish in under 0.1 s.

**Wide parent-read columns.** A 512-byte payload the parent reads back is late-materialized from the stored table, so
it never enters the breaker: at 1,000,000 rows direct takes 0.12 s and 88 MiB against the solver's 0.63 s and 1.7 GiB
(5.2x), and an unused payload takes 0.08 s and 47 MiB. The speedup is smaller than S1's because the solver is already
cheap on a bound-only problem. Footprint numbers for both rules are in `../../architecture/result_adapter.md`
("Memory of holding rows").

**Reproduce** (after `make release`; the sweeps above took under a minute in total, building the databases included):

```sh
python3 benchmark/decide/direct_solve/archive/profile_direct_a1.py --rows 100000 1000000 --db-dir /tmp/a1db
python3 benchmark/decide/direct_solve/archive/profile_direct_a1.py --rows 5000000 --repeats 3 --db-dir /tmp/a1db \
  --shapes real_quadratic real_quadratic_when int_linear bool_bounds constant_problem count_only
```

The runner pins `require` for direct and `off` for the solver, and runs the solver at a size only if its previous size
took at most `--solver-budget` seconds. The 5,000,000-row database needs only the narrow table; build it by hand to skip
the wide one.
