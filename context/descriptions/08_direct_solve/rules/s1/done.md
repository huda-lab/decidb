# S1 (top-k and cardinality intervals) — what works today

Verified on 2026-10-13 on top of `161ba4dbf0`: `make decide-test` (3,346 passed, also with
`DECIDB_TEST_DIRECT_SOLVE=off` and `DECIDB_VERIFY_SERIALIZER=1`), `DECIDB_FORCE_SOLVER=highs make decide-test` (3,345
passed, 1 unrelated failure that needs Gurobi), and `build/release/test/unittest "[decidb]"` (1,319 assertions in 46
cases). `test/decide/run_direct_matrix.sh` renews these numbers in one command (it prints one line per check). Code:
`src/optimizer/decide/direct/s1_rule.cpp`, with the helpers only S1 uses in `s1_uniform_matroid_helpers.{hpp,cpp}`.
Class definition: [definition.md](definition.md).

## What it admits

One row-scoped `BOOL` decision `x` (output is `INTEGER` 0/1), with:

- **Count bounds on `SUM(x)`:** `<=`, `<`, `>=`, `>`, `=`, or several combined into one interval. A bound can be a constant,
  a foldable constant expression, or a numeric source column or source-only expression that varies per row (including
  `COALESCE` and `TRY_CAST`). Values are converted to DOUBLE as the solver does; strict and fractional limits become
  inclusive integer counts; limits are exact up to 2^53.
- **Bounds that count rows:** a bound built from `COUNT(*)`, constants and `+ - * / //` (`SUM(x) <= COUNT(*) / 2`,
  `>= COUNT(*) * 0.1`, `<= COUNT(*) - 1`, `-COUNT(*) + 6`), read per group. `COUNT(*)` counts the rows the clause covers in
  each group: a clause-level `WHEN` or a NULL `PER` key narrows it, while an aggregate-local `WHEN` on the left
  (`SUM(x) WHEN active <= COUNT(*) / 2`) does not narrow the right, which counts every row of the group. The expression is
  evaluated in its own types (a bound of `COUNT(*) * 0.57` over 100 rows is exactly 57) and then read as any source bound.
  A divisor must be a nonzero constant. The use case is a top-fraction query ("the best 10% of each group":
  `SUM(x) <= COUNT(*) * 0.1 PER g`), which is the natural way to write it. The rule proves only the shape (plain `COUNT(*)`,
  exact finite constants, casts that cannot throw, `+ - * / //`). It proves no range. The plan evaluates the bound with
  DuckDB's own operators, so an overflow (`COUNT(*) * 4611686018427387904` over 3 rows) raises the same error as on the
  solver path, and it evaluates the bound only for the groups the solver evaluates it for: a group with a row the clause
  covers (WHEN holds, PER key not NULL). A row outside that reads the bound as 0 and the group extremum ignores it, so a
  NULL-key group or a group the `WHEN` excludes cannot raise where the solver does not.
- **Bounds read from a column of the group:** `MIN`, `MAX` and `COUNT(col)` of one numeric source column, and `SUM` and
  `AVG` of one `TINYINT`, `SMALLINT`, `UTINYINT` or `USMALLINT` column, alone or inside the same arithmetic as `COUNT(*)`
  (`SUM(x) <= MIN(cap) PER warehouse`, `>= MAX(lo) / 2`). Each is one value per group, over the same rows `COUNT(*)`
  counts: the clause's `WHEN` and `PER` narrow it, a left-side aggregate-local `WHEN` does not. Values are read as
  `DOUBLE`, as the solver reads them; `AVG` is that sum over the count; an infinite value is a value. A NULL or NaN in the
  column raises on every row, including rows the `WHEN` or a NULL key excludes, and so does a NULL under `COUNT(col)`,
  which the solver reads as a bad value rather than skipping. A group with no covered row raises the empty-aggregate
  error. `SUM` and `AVG` stop at 16-bit integers because the solver adds row by row in `DOUBLE`: for these types every
  running total stays exact, so the solver's sum equals the exact sum a window computes, and for wider types it does not.
- **Count bodies:** `SUM(a*x + c)` where the terms in `x` add up to exactly `a = 1` or `a = -1` (`SUM(x)`, `SUM(1*x)`,
  `SUM(2*x - x)`, `SUM(-x)`) and `c` is a finite foldable constant, whole or fractional, of magnitude at most 2^21
  (`SUM(x + 1)`, `SUM(1 - x)`, `SUM(x + 0.5)`, `SUM(x + 3 - 1)`). `SUM(x + c) <op> B` is `SUM(x) <op> B - n*c`, and
  `SUM(c - x) <op> B` is `SUM(x) <flipped op> n*c - B`, where `n` is the number of rows the `SUM` covers in the group:
  the `WHEN` holds, whether the clause's or the aggregate's own, and no `PER` key is NULL. The aggregate-local `WHEN`
  narrows `n` even though it does not narrow a `COUNT(*)` on the right. The shift is computed in DuckDB's own types
  (integers widened to `HUGEINT`), so a decimal offset stays exact: 100 rows of `x + 0.57` shift the limit by exactly 57,
  where a `DOUBLE` sum gives 56.99999999999999. The cap of 2^21 keeps `n*c` exact up to 2^32 rows, which the plan
  cannot check because `n` is known only at run time. The solver reads these bodies as the same arithmetic, strict and
  fractional bounds included.
- **Scope:** global, or `PER` one or more source columns, optionally with a deterministic source-only `WHEN` (top level or
  inside the aggregate). All count clauses must share identical membership. Rows with a NULL `PER` key or a false aggregate
  `WHEN` bypass the bound but still obey per-row pins.
- **Per-row bounds on `x`:** any comparison (`=`, `<>`, `<`, `<=`, `>`, `>=`) against a foldable constant, a numeric source
  column, or a deterministic nonthrowing numeric expression over source columns (`COALESCE`, `TRY_CAST` of a column), with
  an optional source-only `WHEN`. Each row's value is read as the set of {0, 1} it allows. Both allowed means no
  constraint (`x <= 1`, `x >= 0`, `x BETWEEN 0 AND 1`, `x <> 2`); one allowed fixes the row to that value (`x >= 0.5`
  fixes to 1); none allowed makes the row infeasible. A bound with a `WHEN` takes any constant. A constant bound without a
  `WHEN` is read the same way when it can hold: a lower bound below 0 (`x >= -3`, `x > -3`) allows both values, because
  the solver keeps a Boolean's box at [0, 1] and a written bound only narrows it. One that cannot hold is left to the
  solver (see the out-of-scope table). A strict bound against a fraction is first shifted to a whole number by
  `DecideStrictBound`, the function the solver uses (`x > 0.5` is `x >= 1`, `x < 1.5` is `x <= 1`, `x < 0.5` is `x <= 0`, `x > -0.5` is `x >= 0`), and then read
  like any other bound;
  a column value is read as arithmetic with or without a `WHEN`, as the solver does. A source Boolean column is also a pin
  value (`x = flag`). Contradictory active bounds are infeasible. A NULL or NaN column value raises on every row,
  including rows the `WHEN` excludes, and names the column.
- **Objective:** a signed sum of per-row coefficients times `x`, `MAXIMIZE` or `MINIMIZE`, plus an optional finite constant,
  or none. Without one every row scores 0, so the plan picks only the rows the lower limits and pins require.
  Coefficients are numeric, decision-free, deterministic; several terms must be non-throwing. Each `SUM(...)` may carry a
  constant factor or divisor (`2 * SUM(score*x)`, `SUM(score*x) / 2`, `-1.5 * SUM(...)`, `(1 + 1) * SUM(...)`) that scales
  that part's coefficients: a negative one reverses the part, zero makes every row score 0, and each part keeps its own.
  The factor must be a finite foldable constant, and a divisor must not be zero.

It raises an error wherever the solver does: empty scoped aggregate, invalid or NULL bounds (including on bypassed rows),
NULL or non-finite scores, infeasible counts. Every input row is read, so a late bad value raises under `LIMIT 1` or
`COUNT(*)`. When a query has several errors, direct may report a different one than the solver. When scores tie it may pick
different rows than the solver, with the same objective. Without an objective every feasible assignment is optimal, so
which rows it picks is not specified; Gurobi and HiGHS pick different ones as well. The feasibility check is a flat list of independent checks (a
pin conflict, a valid interval, enough rows, a fixed count within the upper limit, an unrepresentable limit), each true or
the infeasible error; which failing one raises is not specified, and all of them say the same thing.

**Known difference: values within the solver's tolerance of an integer.** Direct solve reads every
bound exactly: counts, constants and column values. The solver accepts a violation of about 1e-6, and the two backends
differ from each other, so there is no single behavior to copy. `SUM(x) <= 1.9999999` selects 2 rows on the solver and 1 on
direct; `x >= 0.000001 WHEN id = 2` fixes the row on Gurobi but not on HiGHS, and `x <= 0.999999` fails inside HiGHS. A
column value computed in floating point (`0.1 * 10`) can land just under 1 and be read as below 1 by direct and as 1 by the
solver. Chosen over declining near-integer bounds because a column value is only known at run time, where the plan cannot
decline it. To avoid the difference, round the bound. `test_s1_direct_reads_bounds_exactly_near_an_integer` pins the
behavior, so changing it is deliberate.

**Known difference: an offset against a limit near the top of `HUGEINT` or `DECIMAL(38)`.** With an offset in the count
body, the shift `B - n*c` is computed in the limit column's own type. A `HUGEINT` or 38-digit `DECIMAL` value within `n*c`
of its type's maximum (around 1.7e38 and 1e38) overflows and raises, where the solver reads the value as `DOUBLE` and
answers. Accepted rather than declined because no realistic limit is that large. `BIGINT` and smaller types reach the
solver as `DOUBLE` and cannot overflow.

**Error wording** (`../../architecture/policy.md`). The messages are the solver's, built by the same functions. A NULL
count bound, pin or `SUM` source column that is one column raises `DECIDE: column "cap" is NULL. Impute it with
COALESCE(cap, 0) or filter those rows out with a WHERE clause.`, and a NaN one `DECIDE constraint right-hand side contains
invalid value (NaN)...`; the solver adds `at row N` to the second. A NULL score names the NULL columns of the score as
the solver does (`column "score"`, or `columns "a" and "b"`), and quotes the score only when no column is NULL. The
empty-aggregate, non-finite score and infeasible messages are the solver's too. Two cases keep direct solve's own
wording: a bound or pin that is a computed expression (`TRY_CAST(cap AS INTEGER)`) raises `the bound expression has an
invalid value (NULL or NaN)`, since the check counts bad rows over the whole input and has no failing row to name
columns from; and an `=` bound that varies within a group raises `source-valued equality bound varies within a group`,
where the solver names the first two values it meets. Both paths raise on the same inputs.

## What it does not admit (falls back to the solver)

A query S1 does not apply to keeps its answer: it runs on the solver and `require` names the clause.

**Explicitly out of scope** (these stay on the solver on purpose):

| Shape | Why it is out | How to reopen it |
|---|---|---|
| `SUM` or `AVG` on the right of a count bound over a column wider than 16 bits: `INTEGER`, `BIGINT`, `HUGEINT`, their unsigned forms, `DECIMAL`, `FLOAT`, `DOUBLE` | The solver adds row by row in `DOUBLE`, so a running total past 2^53 or a fraction loses digits in an order-dependent way: over the `BIGINT` values 2^60, 1, -2^60 it reads the sum as 0 where the exact sum is 1. A window sum adds in another order or exactly, and a last-digit difference can move a limit across a whole number | Read the solver's sum in the solver's row order, or prove a column's sums exact for the group sizes seen |
| A right-hand aggregate with its own `WHEN` (`SUM(x) <= COUNT(*) WHEN active`), `DISTINCT` or `ORDER BY` inside it, a reducer of an expression rather than one column (`MIN(cap * 2)`), an aggregate as a divisor, or another reducer (`MEDIAN`) | Kept on the solver by decision | A separate task per shape |
| A count body whose added term is not a usable constant: a column (`SUM(x + bonus)`), NULL, NaN, an infinity, a constant above 2^21 in magnitude, or a weight on `x` other than 1 or -1 (`SUM(2*x + 1)`) | A column term adds a different amount per row, so the shift is not `n*c`. A non-finite or NULL term raises on the solver. Above 2^21 the plan cannot prove `n*c` exact. A weight other than ±1 is a weighted constraint (S3, S4) | Raise the 2^21 cap with a run-time check of `n` |
| A bound expression that can raise at runtime, such as a narrowing `CAST` | The solver raises on every row, including rows a `WHEN` or NULL `PER` key excludes, and direct solve must raise on the same inputs. Admitting it means proving DuckDB does not skip the check on a row the solver reads. Nonthrowing forms (`COALESCE`, `TRY_CAST`) are admitted, so `TRY_CAST` is the workaround. A multi-term objective whose coefficient can throw is left to the solver for the same reason | Prove the validation stage evaluates every row, then admit expressions that can only raise a cast or overflow error. Low value and costly |
| A factor on `SUM(...)` in the objective that is not a usable constant: a zero divisor, a NULL factor, or a subquery factor (`(SELECT 2) * SUM(...)`) | A zero divisor raises on the solver, which a plan built from a constant cannot reproduce. The solver reads a NULL factor as an objective of zeros, which is not a result worth copying. A subquery factor has no sign when the plan is built, and the sign decides whether the sense reverses | Admit a subquery factor once its value can be read before the plan is chosen. Low value |
| A bound on `x` with no `WHEN` that cannot hold: an upper bound below 0 or a lower bound above 1 (`x >= 2`, `x <= -1`, `x < 0`, `x > 1.5`, `x = -1`; a strict bound counts after its shift to a whole number) | The solver raises DECIDE's usual infeasible error before it reads a row, so an empty source raises it too, and skips that error under `DIAGNOSE`. A query that can only fail has no speed to win, so S1 declines it (`DecideBoxContradictsDomain`) and the solver answers. The same bound with a `WHEN` is admitted | Not planned |

**Belongs to another problem class or is intentional:** weighted constraints such as `SUM(w * x) <= B` (S3 and S4 cover two special cases of them); `INT` and
`REAL` variables (A1 and the R classes); more than one decision, entity or scalar decisions (D1); lower and upper bounds with
different `PER` or `WHEN` membership (S2); `SUM(x) <> k` (not an interval); bounds that are infinite, NaN or above 2^53
(not exactly representable); objective `PER` or `WHEN`, other reducers (`MIN`, `MAX`, `AVG`), `norm`; `DIAGNOSE`.

## Verified by

| Check | What it covers |
|---|---|
| `test_direct_three_way.py` (1,131 tests) | The answers. Each case runs on the independent ILP oracle (`oracle_solver`, built from the raw rows), on the solver path (`off`) and on the direct path (`require`, so the test fails if S1 does not apply); both DeciDB runs must satisfy every constraint and reach the oracle's objective, or all must say infeasible. Cases: every comparison against whole and fractional limits (strict bounds as the solver path reads them), intervals, `PER` and NULL keys, top and aggregate-local `WHEN`, source-valued bounds and `COALESCE`, several bounds at once, row-count bounds (`COUNT(*)`, exact decimal arithmetic, and `test_s1_count_bound_raises_only_where_the_solver_does` for where an overflow may raise), group-aggregate bounds (`MIN`, `MAX`, `COUNT(col)`, small-integer `SUM` and `AVG`, in arithmetic, with `PER`, top and local `WHEN`, their NULL, NaN, infinity and empty-group errors, and every declined reducer), offset and negated count bodies (each comparison and an interval, whole and fractional offsets, every scope, a group whose local `WHEN` covers no row, an unreachable limit, constant, column, `COALESCE`, `COUNT(*)` and group-aggregate limits, exact decimal shifts, limits near the `BIGINT` limit, and every declined body), pins and per-row bounds on `x` (a generated grid with and without `WHEN`), column pins, no objective (every count bound shape, with and without `PER` and `WHEN`, checked for feasibility only), 11 objective shapes (sums, differences, negation, scaled and divided, offset, products), the nine beyond the plain score in both senses, tied scores, and infeasible problems. Parent-query contexts (CTE, wide source, joined and correlated source, parent join, recombined filters), a nested `DECIDE`, and empty inputs. Error cases that must fail on both paths (bad scores, empty aggregates, bad bounds on rows the problem does not read, late rows under `LIMIT` and `COUNT`, pins). Boundary cases against the solver path only (infinite and beyond-2^53 limits, every numeric source type, exact decimal arithmetic) and tiny scores against the exact finite-DOUBLE optimum |
| `test_direct_rule_contract.py` (107 tests, shared with A1) | The checks every rule owes: schema and rows, all-rows read, serializer, prepared plans, `EXPLAIN` and profiling, `off`, forced backend, `DIAGNOSE`, an empty source, constant bounds that contradict the built-in domain, and near misses, each of which must name its reason, keep the solver plan, and answer under `auto` as under `off` (this is where the shapes S1 leaves on the solver are tested) |
| `test_direct_fuzz.py` (24 tests, 8 seeds per rule) | Seeded differential fuzz: random small S1 queries with and without an objective, direct (`require`) against solver (`off`), over per-row bounds (including lower bounds below 0 with no `WHEN`), scaled objectives, row-count bounds and offset or negated count bodies. Finds interactions the tables do not list |
| `test_direct_user_facing.py` (53 tests, shared with A1) | What a SQL user sees besides the answer: direct solve on by default, the pinned exact reading of a bound near an integer, the full text of every NULL and NaN error compared with the solver's, unused wide columns pruned, forced-backend and binder-error policy |
| C++ `[decidb]` | S1 applicability condition, IR, rule driver |

## Performance

Measured 2026-10-13 (macOS arm64, 11 cores): the global narrow rows on top of `95ee8bf58c` with only variable-width columns late-materialized, the rest at `7a480a1a71`, where those shapes plan the same way. 5,000,000 stored rows; every query ran in a fresh process
that only opens the database file, so peak memory is the query's own. Times are median query-plus-collection seconds
(direct 1 to 3 runs, Gurobi 1 or 2). In all 36 runs direct and Gurobi agreed on selected count and objective. HiGHS was
not re-run; earlier it took about 43 s on the global shape.

| Shape | Direct | Gurobi | Speedup | Direct peak | Gurobi peak |
|---|---:|---:|---:|---:|---:|
| Global top-K, narrow, full output | 0.18 s | 2.89 s | 16x | 1.0 GiB | 2.3 GiB |
| Global top-K, narrow, every column reduced in the query | 0.17 s | 2.84 s | 17x | 0.9 GiB | 2.2 GiB |
| Global top-K, unused 512 B payload (pruned) | 0.16 s | 3.00 s | 19x | 0.8 GiB | 5.3 GiB |
| 100 `PER` groups, fixed pins | 0.33 s | 32.4 s | 98x | 0.5 GiB | 6.6 GiB |
| 100 groups, aggregate-local `WHEN` | 0.30 s | 28.4 s | 95x | 0.5 GiB | 6.0 GiB |
| 100 groups, local `WHEN`, source-valued bound | 0.55 s | 27.4 s | 50x | 0.7 GiB | 3.7 GiB |
| 100 groups, source-valued bounds | 0.50 s | 29.6 s | 59x | 0.7 GiB | 6.3 GiB |
| 100 groups, paired source-valued bounds | 0.66 s | 29.6 s | 45x | 0.8 GiB | 6.3 GiB |
| 100 groups, `COALESCE` source-valued bound | 0.57 s | 29.5 s | 52x | 0.6 GiB | 6.3 GiB |

The two global narrow rows return the `BIGINT` key `i`, which stays in the single read (only a variable-width column is
late-materialized; `../../architecture/result_adapter.md` § Late materialization of a stored table). Gurobi's peak
varies by about 2 GiB between identical runs (4.6 to 6.5 GiB on the 100-group shape over four runs): the model it
receives is the same in every run, and the peak is reached inside its presolve, which takes 32 of the 33 s. DeciDB's own
part of those runs peaks at 1.2 GiB.

The speedup comes mostly from the solver being slow here: it builds a model with millions of variables, which direct solve
skips. A shape whose solver model is cheap would show a smaller win.

**Without an objective** (`SUM(x) BETWEEN rows/20 AND rows/10`, one aggregate row, direct 3 runs, solver 1): 0.05 against
0.07 s at 100,000 rows, 0.06 against 0.28 s (85 against 325 MiB) at 1,000,000, and 0.16 against 1.23 s (541 to 657 MiB
against 1.6 GiB) at 5,000,000.

**The NULL check.** Every score is checked for NULL on every run, and the check cannot be switched off, so its cost on
clean data is part of every number above and is not measured apart. With one NULL score on the last row, direct raises
in 0.04 s at 1,000,000 rows and 0.07 s at 5,000,000 (clean: 0.06 and 0.19 s), and the solver in 0.06 and 0.12 s.

**Weak spots**

| Case | Result |
|---|---|
| Wide rows (512 B) returned in full | Direct 0.46 s versus Gurobi 3.40 s, 7.4x; reduced inside the query, 0.46 against 2.95 s. The payload is late-materialized from the stored table, so it never enters the rank. Memory 3.9 GiB versus 6.2 GiB with the full result collected, 1.4 GiB versus 5.4 GiB when reduced. Footprint numbers are in `../../architecture/result_adapter.md` |
| Small inputs, up to 10,000 rows | Everything finishes in about 20 ms, so no real difference (earlier measurement, not re-run) |
| 100,000 wide rows, trivial capacity | Direct about 5% slower than Gurobi, two runs (earlier measurement, not re-run). The only measured loss |
| Capacity 0 or all rows, 5M narrow | Still wins, by 3 to 4x instead of 16x (earlier measurement, not re-run) |

**Reproduce** (after `make release`; the sweep above took about 4.5 minutes):

```sh
clang++ -std=c++17 -O2 -I src/include benchmark/decide/profile_direct_s1_api.cpp \
  -L build/release/src -lduckdb -Wl,-rpath,$PWD/build/release/src \
  -o benchmark/decide/results/profile_direct_s1_api
P="python3 benchmark/decide/profile_direct_s1_api.py --rows 5000000 --db-dir /tmp/s1db --keep-db --timeout 180"
$P --widths 0 512 --output-kinds full consume --modes direct gurobi --repeats 2
$P --widths 512 --output-kinds aggregate --modes direct gurobi --scopes global --cardinalities upper --repeats 2
$P --widths 0 --output-kinds aggregate --modes direct gurobi --scopes grouped_local_when --cardinalities interval --bound-kinds constant source --repeats 1
$P --widths 0 --output-kinds aggregate --modes direct gurobi --scopes grouped --pins fixed --cardinalities interval --repeats 1
$P --widths 0 --output-kinds aggregate --modes direct gurobi --scopes grouped --cardinalities interval --bound-kinds source source_pair source_coalesce --repeats 1
```

Add `--output file.csv` to keep the raw rows. The runner pins `require` for direct and `off` for the solver, so a run never
silently uses the other path. The wide database needs about 3 GiB of disk.
