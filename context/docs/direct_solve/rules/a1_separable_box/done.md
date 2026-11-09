# A1 (separable box-constrained problem) — what works today

Verified on 2026-11-08 at `73b52ab1d1`: `test/decide/run_direct_matrix.sh`, each of its four suites (auto, off,
serializer, highs) 7,020 passed; C++ `build/release/test/unittest "[decidb]"` 4,635 assertions in 85 cases. Code:
`src/optimizer/decide/direct/a1_separable_box_rule.cpp`; its per-row bounds and the terms of its objective come from the
shared reader in `direct_bound_reader.cpp`. Class definition: [definition.md](definition.md).

## What it admits

One or more row-scoped `BOOL`, `INT` or `REAL` decisions, each solved on its own, with:

- **Bounds, per row.** Any comparison (`=`, `<`, `<=`, `>`, `>=`, `BETWEEN`, either side) of a decision against a constant,
  a numeric or Boolean source column (a Boolean is read as 0 or 1, for `BOOL` and `INT`), or a deterministic expression
  over source columns. The decision may carry a coefficient and an offset (`2*x + 1 <= cap`, `-x >= -cap`, `w*x <= cap`);
  a row-valued coefficient picks the side by its sign, and a zero coefficient leaves the row alone or infeasible. Each
  clause may carry its own `when`. The value expressions may raise at run time (an overflowing cast, a division by
  zero): the plan evaluates them on every row the clause's `when` keeps, as the solver does, and computes nothing on a
  row the `when` drops (syntax reference §6.3). Each clause reads as the lower and upper end of the values it admits,
  the two expressions the solver path's classifier builds (`DecideReadSingletonRow`), so an equality is one clause
  with both ends set.
- **Default box.** `BOOL` is [0, 1]; `INT` and `REAL` are [0, ∞). An unconditional constant lower bound replaces the
  default 0 on the solver, so it does here, in any spelling the classifier reads as a bound on one decision (`x >= -3`,
  `2*x >= -6`, `x + 1 >= -3`, `x >= 0 - 3`, D41). A constant lower bound under a `when` replaces it on the rows the
  `when` keeps: the decision's static floor on a row is −∞ where any such `when` holds and 0 elsewhere, a NULL `when`
  keeping the 0 (`DirectStaticFloor`), and the clause's own bound column gives the value. A decision with a lower bound
  written, or a `BOOL`, keeps its floor. A lower bound from a column only intersects with the default. The solver and
  direct solve read which clauses are folded into the variable from the same classifier
  (`DirectIRConstraint::variable_bound`), which clauses replace the default under a `when` from the same predicate
  (`DecideSingletonRow::LiftsDefaultFloor`), and combine the folded values with one function (`DecideFoldSingletonRow`),
  so they cannot disagree. R1 reads its box through the same reader and static floor. A bound of magnitude 1e20 or more is no bound, as on the solver, including a written -1e30 and a
  literal `-inf` floor. An infinite data value is no bound in the open direction (`x <= cap` with `cap = +inf`) and
  admits nothing in the other (`x >= cap`), which empties the row's box (D65).
- **Whole-number rounding.** An `INT` or `BOOL` decision admits the whole numbers the written comparison admits as
  DuckDB evaluates it (D64), strict bounds included: `0.1*x <= 0.3` admits 3 (the DECIMAL product is exactly 0.3), and
  `x > 0.5`, `x < 2.5` admit 1 and 2 at their ends. The six strict cases are in `SOLVER_OFF_BY_ONE`
  (`test_direct_a1_separable_box.py`), checked against the oracle on both paths; `test_singleton_row.py` checks the
  DECIMAL and DOUBLE spellings against DuckDB on both paths and both backends. S1 reads the same interval on a `BOOL`.
- **Objective.** `MINIMIZE`, `MAXIMIZE`, or none (the lowest allowed value). Signed `SUM` parts, each with an optional
  constant factor or divisor and an optional `when`, plus an optional objective-level `when`. A part's values are
  computed only on the rows its `when` and the objective's keep. Terms: a row-valued
  coefficient times a decision, a per-row constant, and at most one square `k*(w*x + t)^2`, where `k` is a nonzero
  constant and `w` and `t` are row-valued (`w` may be zero or negative). `POWER(x - t, 2)`, `(x - t)*(x - t)`,
  `x*x - 2*t*x`, `norm(e, 2)` and a constant factor inside or outside the `SUM` are all this shape. A per-row
  constant changes no answer: as on the solver path, each is checked on its own, with the part's factor applied, and
  none is added up.
- **Per row, the optimum** is an endpoint or the clamped turning point. For whole numbers the turning point is replaced
  by the first integer whose next step no longer improves, so a tie goes to the smaller value (as on the solver). Both
  are computed with the halves taken first (`(-0.5 * b) / a`, `ceil((-0.5 * b - 0.5 * a) / a)`), so a square factor up to
  the top of a DOUBLE keeps its turning point, where `2 * a` would pass one: at 1e308 the answer is the factor-1 query's.
  Maximizing a convex square and minimizing a concave one compare the two endpoints.

A NULL coefficient or bound skips its instance, as on the solver. It raises an error wherever the solver does: NaN or
infinite coefficients and NaN bounds on a row that reads them, an empty box on any row (infeasible), an improving direction with no finite limit on any row (unbounded), an
`INT` answer beyond 2^53 after rounding (with the solver's message word for word, naming the variable and its rounded
value), and any expression that raises on a row that computes it. A term whose `when` keeps no row is dropped, as on the
solver (D68). Every input row is read before one is released, so a late bad row raises under `LIMIT 1`,
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
| A second squared term, a data weight outside a square (`w*POWER(...)`), `w*x*x` | A1 reads one square and folds its weight to a constant (`DirectFoldConstantSum`), so it declines a weight read from the data. The solver path answers these on Gurobi (a data weight on a square is Gurobi only). |
| Strict bounds on a `REAL` | The binder rejects them before any rule runs. |
| A bound that contradicts the variable's built-in domain (`x <= -1` on a non-negative decision, `BOOL x >= 2`) | The solver rejects it before it reads a row, even on an empty source, so the solver reports it. |

## Where it differs from the solver

- **A square's constant past a DOUBLE** (`SUM(POWER(x - v, 2))` with `v = 1e200`): the solver path expands the square
  and refuses once its constant `k*t^2` passes a DOUBLE; A1 answers `x = v`, which never reads that constant
  (`../../architecture/rules.md` § What every rule must match from the solver).
- **Values within the solver's tolerance of an integer** (`INT x <= 1.9999999`). Direct solve reads every bound exactly,
  as in S1 (`../s1_uniform_matroid/done.md`); the solver accepts a violation of about 1e-6 and returns 2. The tests use values that are
  exact in a double, so they never sit in that band.
- **Error wording.** An empty box on a row reads `DECIDE optimization is infeasible. Prefix the query with DIAGNOSE
  ...` here. A bound that contradicts the built-in domain is left to the solver (see above), so its own message appears:
  for a non-negative decision it quotes the clause as typed (`x < 0 cannot hold because x is non-negative`), and for a
  `BOOL` too (`x >= 2 cannot hold for a BOOL variable.`). A NULL coefficient, target, weight or bound names the columns that are NULL
  on the failing row, in the solver's text, and a NaN value reads as the solver's message; both are built by the
  functions the solver calls (`decide_value_messages.hpp`).
- **Its own warning.** The divisor is a decision's square factor `a`, which the turning point `-b / 2a` divides by; the
  rule is the framework's [Number-range warning](../../framework/done.md). `MINIMIZE SUM(1e-322 * POWER(x - 1/3, 2))`
  answers 0.35 and warns. At 1e-300 it answers 1/3 and is silent, where `off` warns and returns 0 (Gurobi) or
  0.33333333331 (HiGHS). With no square the answer reads only the sign of `b`, so A1 is silent there.
- **Ties.** The smaller value, which matches the solver in every case checked, but nothing promises it.
- **A decision that appears in no objective term** takes the lowest value its box allows.

## How it works

One projection per stage of `definition.md` § Relational construction, with no sort, no window and no join, and no row
held. A raw value is NULL on a row its `when` drops (`DirectOnKeptRows`), so its check passes there.

The solver reads every row before it returns, so a bad value on the last row raises even under an outer `LIMIT 1`. A1
keeps that by verifying, then streaming: one read of the source runs stages 1-4 into a one-row verdict, the `bool_and`
of every row's check (`DirectAllRowsValid`), true or raising, and the source reader gates the second read on it
(`DirectSourceReader::Gate`, after the row checks' own gate). The second read runs stages 1-4 again and releases each
row as it comes; nothing is held, and unreferenced source columns are pruned from both reads. A problem made only of
constants, with no `when`, fails the same way on every row, so it takes one read: the check is a projected validation
column on that read.

**The row box.** The box of a decision on a row is its static box intersected with the bounds of the clauses the row
keeps (`DirectRowBox`, the framework's [bound reader](../../framework/done.md)). Its lower end is the `GREATEST` of the
static floor and the clauses' lower ends, and its upper end the `LEAST` of the static upper end and the clauses' upper
ends. A static end that is infinite is left out, since it clamps nothing: a decision with no finite upper bound has no
constant in its `LEAST`, and one with a floor of −∞ has none in its `GREATEST`. With no end left on a side, the box is
open there. The static floor is a column of its own only when a `when` can lower it (`DirectStaticFloor`, which is −∞
on the rows where one holds). With no such `when`, the floor is the constant itself and the plan computes nothing
for it.

**Checks and sums.** A value of an exact type (an integer or DECIMAL column) plans no invalid-value check, since its
type holds no NaN or infinity, and a bound whose right side has an exact type no NaN check; a FLOAT or DOUBLE column, a
part factor that overflows the type, and every too-large check of a number the plan computes keep theirs (the
framework's [Number messages](../../framework/done.md)). A check gates on `isfinite` and builds its message only where
it raises. The objective's sums start from their first term, with no `0.0` seed, and the plan is canonicalized after it
is built (constant columns propagated, DuckDB's expression rewriter over it).

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_a1_separable_box.py` (196 tests) | The answers. 104 cases run on the independent oracle (`oracle_solver`, built from the raw rows and the per-row bounds each case states in Python), on the solver path (`off`) and on the direct path (`require`): constant boxes, the default-lower rule in every spelling that does and does not replace it, row-valued bounds with and without `when`, coefficient forms (positive, negative, zero), `INT` rounding and strict bounds, `BOOL` pins and Boolean-column bounds, every objective spelling, objective `when`, infeasible and unbounded problems, ties, and three decisions of different types. 14 parent-query contexts. 5 error cases that must fail on both paths. 21 boundary cases against the solver path only (infinities, the 1e20 and 2^53 limits, every numeric source type). 12 cases on how bounds combine, on both paths: a written lower bound of -1e30 or -1e31 is no bound for `INT` and `REAL`, a second lower bound beside it still applies, and no written lower bound leaves the floor at 0. 6 random-row tests against exact `Fraction` arithmetic. The constant-problem tests (streaming plan, errors under `LIMIT 1`, `COUNT(*)` and a filter, empty source), a nested `DECIDE`, and duplicate rows The affine spellings of a folded bound run on both paths. 6 cases of the number-range warning on the direct path: a square or linear coefficient below the normal range warns, on a later decision too, and coefficients in it, a square with no linear term and a linear term alone are silent. |
| `test_direct_rule_conformance.py` | The checks every rule owes, with A1's fixture: schema and rows, all-rows read for 10 kinds of late bad row, serializer, prepared plans, `EXPLAIN` and profiling, `off`, forced HiGHS, `DIAGNOSE`, and 17 near misses that must decline under `require` and answer under `auto` as under `off`, and a plain form that applies |
| `test_direct_user_facing.py` | The plan over NOT NULL columns holds no check the statistics prove false; each infinite static end is absent from the plan's `LEAST` or `GREATEST` and each finite one present, and a NaN upper end still raises with no static end to clamp it; a 200-byte unused payload column stays out of both reads; a computed column that can raise is not pruned; a NaN in a constraint coefficient says "constraint coefficient" and in the objective "objective coefficient"; every NULL and NaN error reads as the solver's, compared in full |
| `test_error_infeasible.py` | The solver's rejection of a bound against the built-in domain quotes the clause as typed for a non-negative decision (`x <= -1 cannot hold ... non-negative`, strict `x < 0`); for a `BOOL` (`x >= 2`, `x > 1`, `x > 1.5`) it is the usual infeasible error |
| Five tests pinned to the solver path | `test_canonicalize_idempotence.py` (three cases) and `test_implied_bounds.py` (two cases) inspect the solver's model dump. Direct solve builds no model, so they run with `DECIDB_TEST_DIRECT_SOLVE=off` on purpose. |
| C++ `[decidb]` | `variable_bound` over 19 spellings, the IR for every objective spelling, A1's applicability condition and its declines, S1 and A1 never both applying, and the rule driver choosing the right real rule for each shape |

## Performance

Measured on 2026-11-09 at `2230755259`, on an 11-core Apple M3 Pro at a load of 5 to 6.

**Against a hand-written plan.** `benchmark/decide/direct_solve/run.py --compare --sf 1 --queries a1` runs the direct
plan beside a query written by hand for the same answer (`hand/a1.sql`), with the same checks and the same DOUBLE
arithmetic, in one process, best of 5, over the 800,000 rows of `partsupp`. The hand plan owes what the rule owes: its
verdict over every row before the first row is released, so it reads the table twice too. CPU is every operator's time
summed; latency is on all threads; the planning DECIDE adds is apart. Both sides gave the same answer.

| Query | Direct CPU | Hand CPU | CPU ratio | Direct latency | Hand latency | DECIDE planning |
|---|---:|---:|---:|---:|---:|---:|
| `a1` | 29.7 ms | 25.9 ms | 1.14 | 7.3 ms | 5.4 ms | 0.50 ms |

**Against the solvers.** `run.py --sf 0.125 0.25 0.5 1 --queries a1`: a fresh `decidb` process per run, median of 3.
At every size Gurobi's objective agreed with direct solve's within 1e-6. A speedup divides Gurobi's median by direct's
median from the same sweep. Times include process start and database open, about 20 ms, which is most of the direct
time at these sizes.

| Rows | Direct | Direct, 1 thread | Direct peak | Gurobi | Gurobi peak | Speedup |
|---:|---:|---:|---:|---:|---:|---:|
| 100,000 | 0.024 s | 0.024 s | 23 MiB | 0.090 s | 87 MiB | 3.8x |
| 200,000 | 0.025 s | 0.027 s | 24 MiB | 0.101 s | 142 MiB | 4.1x |
| 400,000 | 0.026 s | 0.034 s | 26 MiB | 0.189 s | 242 MiB | 7.2x |
| 800,000 | 0.028 s | 0.047 s | 28 MiB | 0.353 s | 456 MiB | 12.5x |

At 800,000 rows direct solve answers 12.5 times as fast as Gurobi in a sixteenth of its peak memory, and all threads
take 0.6 of one thread's time. HiGHS has no cell: its QP solver stops with "Large nullspace" once more than 4,000
decisions end strictly inside their bounds, which A1's do at 10,000 rows (`../../../pipeline/07_solver/done.md`, HiGHS
backend).
