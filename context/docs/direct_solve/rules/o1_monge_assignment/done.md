# O1 (Monge assignment) — what works today

Verified on 2026-11-10 at `8515df17ae`: `test/decide/run_direct_matrix.sh`, each of its four suites (auto, off,
serializer, highs) 6,715 passed; C++ `build/release/test/unittest "[decidb]"` 6,035 assertions in 103 cases. Code:
`src/optimizer/decide/direct/o1_monge_assignment_rule.cpp`. Class definition: [definition.md](definition.md).

## What it admits

A one-to-one matching of the rows of two tables, over their cross product:

```sql
SELECT l.id, r.id, x
FROM left_items l CROSS JOIN right_items r
DECIDE x(BOOL)
SUCH THAT per l: SUM(x) by (l) = 1
AND per r: SUM(x) by (r) = 1
MINIMIZE SUM(POWER(l.position - r.position, 2) * x);
```

- **The source.** DECIDE's own child is a cross product (the IR's `cross_product`). A `WHERE` on one side is admitted:
  filter pushdown has placed it inside that side.
- **The decision.** One `BOOL` per row, with no `per` of its own.
- **The constraints.** Exactly two, each `per K: SUM(x) by (K) = 1` with no `when`, the same key on `per` and `by`,
  every element of the key a column of one side (`per l`, `per l.id`), and the two keys on different sides. A key
  element that stands for a relation takes NULL as a value, so a class is a distinct row of its side and a duplicate
  row shares its class. A key element written as a column is declared `NOT NULL`: a NULL in it puts its row in no
  class, which the plan does not model.
- **The objective.** One term, `k * cost * x`, where `k` folds to a constant (casts, `*`, unary `-` and a scale outside
  the `SUM` fold into one sign, and a `MAXIMIZE` objective reaches the rule as the minimization of its negation) and
  `cost` is one of:
  - `ABS(a - b)` or `POWER(a - b, 2)`, in any of their spellings (`@`, `pow`, `**`, `^`), minimized with `k >= 0`
    (`MINIMIZE k * ...`, `MAXIMIZE -k * ...`): equal ranks;
  - `a * b`: by the rearrangement inequality, equal ranks when the minimized term has `k < 0`, opposite ranks when
    `k > 0`.

  `a` is a numeric column of one side and `b` of the other, in either order, with casts looked through.
- **The positions.** `a` and `b` are declared `NOT NULL` on the tables their sides scan directly (the IR's `not_null`).
- **The key determines the position.** Rows that agree on a side's key agree on its position: the key holds the
  position column, or it holds a key of the position's table (the IR's `keys`). So `per l.id` answers when `id` is a
  key of the table `l` scans, and its classes are `per l`'s.

A table's keys are its UNIQUE constraints, the PRIMARY KEY included, whose columns the scan reads and the table
declares all `NOT NULL`. UNIQUE lets NULLs repeat, and DuckDB declares a PRIMARY KEY's columns `NOT NULL`, so the
`NOT NULL` test covers both (owner's ruling, 2026-11-10). A key is a fact of the catalog, which a direct-solve rule may
read and the canonicalizer may not (`../../../pipeline/04_canonicalizer/done.md` § What a rule must be). DuckDB's
index and `GROUP BY` agree on a DOUBLE key (-0.0 and 0.0 are one value, and so are NaNs); the index ignores a column's
collation where `GROUP BY` applies it, and a collated key is no bare column, so it declines.

**The answer.** Each side's classes are ranked by position; the pair of the left class and the right class of equal
rank (or opposite rank) gets `x = 1`, once per matched class pair, and every other pair gets 0. An empty side gives an
empty result, as the solver's empty model does.

**Errors, as on the solver path.** A NaN or infinite coefficient on any pair raises the solver's error, also under
`LIMIT 1` or `COUNT(*)`. Unequal class counts on two nonempty sides raise the solver's infeasible error.

## How it works

Eager aggregation (Yan–Larson), applied to windows: every step whose keys come from one side runs on that side's own
rows, below the cross product, and the pairs are formed once. `Rule::Apply` takes DECIDE's own cross product from the
reader (`source_reads = 1`, `TakeRecomputedSource`) and builds one `DirectSourceReader` per side, two reads each, so
each side makes the reader's choice between a rescan and a copy of its n rows (a side whose filter calls `random()` is
copied once, and both reads see the same rows).

1. `CheckPairs`: the written coefficient on the cross product of one read of each side, checked by
   `DirectCheckNumber`, in a `bool_and` over every pair (`DirectAllRowsValid`) that gates the left reader (A1's
   verify-then-stream). Where the type proves every coefficient finite, the check still reads the coefficient, so
   DuckDB's own evaluation error (a DECIMAL product that overflows) raises.
2. `Rank`, per side: one window of `ROW_NUMBER`, `RANK` and `DENSE_RANK`, ordered by the position (descending on the
   right side for opposite ranks), then every other key column, so the rows of one class are peers. `DENSE_RANK` is the
   class's rank, the greatest `DENSE_RANK` (a second window, no second sort) is the number of classes, and
   `ROW_NUMBER = RANK` marks one copy of each class.
3. `Assign`, over DECIDE's own cross-product node with the two ranked sides as its children: the source columns, `x` =
   equal class ranks and both copies marked, and the class-count check (`DirectInfeasibleErrorPredicate`) as a
   validated column. Every pair carries the same counts, so the first pair raises.

The right reader is finished outermost, because the left side's verdict reads the right side's copy.

`EXPLAIN` shows `O1_MONGE_ASSIGNMENT` and its summary line.

## What it does not admit (falls back to the solver)

A query O1 does not apply to keeps its answer: it runs on the solver, and `require` raises one fixed error. Open work
on these shapes is in [todo.md](todo.md).

| Shape | Why |
|---|---|
| A maximized distance: `k < 0` in the minimization (`MAXIMIZE k * ABS(a - b)` with `k > 0`) | Opposite ranks are optimal there, but not proved for the rule. |
| `TRY_CAST` around a position | It can make a NULL the table never holds. |
| A position column not declared `NOT NULL`, or read through a projection, outer join or computed expression | A NULL position makes its item free (`SUM` drops the term), and ranks no longer place it. |
| A column key that does not determine the position (`per l.name` with no key on `name`, one column of a composite key), or a key column that may be NULL | Rows of one class may hold different positions, and a NULL puts its row in no class. |
| A `WHERE` reading both sides | It makes a join: not every pair exists. |
| A `when`, a third constraint, fixed pairs, a bound other than `= 1` | Not a plain matching. |
| A second objective term or part, `AVG`, no objective | Not one cost. |
| `INT` or `REAL` decisions, two decisions, a keyed decision | Not one `BOOL` per pair. |

## Where it differs from the solver

- **Ties.** Several matchings can be optimal (tied positions, duplicate rows); O1 may return a different one.
- **No number-range warning.** O1 only ranks and compares positions, so no step of its plan divides or loses precision
  (owner's ruling, 2026-11-10).

## Tests

- Conformance: the `O1` fixture in `test/decide/tests/_direct_rule_fixtures.py` (TPC-H `nation` × `region`; near
  misses that decline, forms that apply, the infeasible late error).
- Differential oracle: `test/decide/tests/test_direct_o1_monge_assignment.py`: every objective form, sign and spelling,
  duplicate rows, a NULL key value, tied, negative and integer positions, one-side `WHERE`s, unequal sizes, against the
  oracle; parent contexts; an empty side; a `WHERE` across both sides and nullable positions keep the solver; NaN
  positions fail on both paths; a NaN on the last of 71 × 71 pairs raises under `LIMIT 1` and `COUNT(*)`. A side
  copied once (left, right, both, both with duplicates) applies and holds the copies `EXPLAIN` counts; duplicate rows
  with a NULL key on both sides; -0.0 against 0.0 and NaN in a key column are one class each, as on the solver path; a
  DECIMAL product that overflows raises on both paths with no check of its own. A negated distance under `MAXIMIZE`
  applies (`test_o1_monge_maximizing_a_negated_distance_is_minimizing_it`). Column keys: a PRIMARY KEY, a UNIQUE
  NOT NULL column, a key that is the position, a composite key written in full, and a column key beside a relation key
  answer under `require`; no key, a nullable UNIQUE column, one column of a composite key and a key with a nullable
  column keep the solver (`test_o1_monge_a_column_key_of_its_table_agrees_with_the_oracle`,
  `test_o1_monge_a_column_that_is_no_key_of_its_table_keeps_the_solver`).
- Spellings: `test_alias_o1_monge_reads_a_square_or_an_absolute_value_under_any_spelling` in
  `test_function_aliases.py`.
- Error text: `test_o1_monge_infeasible_error_is_the_solvers_error_word_for_word` in `test_direct_user_facing.py`.
- C++: `O1AppliesShapes` and `O1DeclineShapes` in `test/common/test_decidb_direct_decline.cpp`, one decline per
  `CheckCondition` branch, and the partition test against every other rule; the IR's `cross_product`, `not_null` and
  `keys` in `test_decidb_direct_ir.cpp`.
- Join order: `test_a_parent_filter_stays_above_a_decide_over_a_join` in `test_nested_decide.py`.

## Performance

Measured on 2026-11-10, on an 11-core Apple M3 Pro at a load of about 5. The query is
`benchmark/decide/direct_solve/queries/o1.sql`: `supplier` paired with itself, one `BOOL` per pair, the least total
product of the paired balances (opposite ranks). Its decision count is the square of the supplier count.

**Against a hand-written plan.** `run.py --compare --sf 0.32 --queries o1` at `804e1efab5`, best of 5, over 3,200
suppliers a side (10.24M pairs), beside `hand/o1.sql`, which ranks each side and forms the pairs once. Both gave the
same answer.

| Query | Direct CPU | Hand CPU | CPU ratio | Direct latency | Hand latency | DECIDE planning |
|---|---:|---:|---:|---:|---:|---:|
| `o1` | 123.6 ms | 43.1 ms | 2.87 | 131.4 ms | 44.2 ms | 0.13 ms |

The rule's extra work is the pass that checks the coefficient on every pair before any pair is returned, and the
copy and class ranks of each side, which the hand plan does not need on distinct supplier rows.

**Against Gurobi.** `run.py --sf 0.01 0.02 0.04 0.08 0.16 --queries o1`: direct solve at `804e1efab5`, Gurobi at
`01bb152842` (the solver path is the same at both), a fresh `decidb` process per run, median of 3. At every size
Gurobi's objective agreed with direct solve's within 1e-6. Times include process start and database open, about 20 ms,
which is most of the direct time here.

| Suppliers a side | Pairs | Direct | Direct, 1 thread | Direct peak | Gurobi | Gurobi peak | Speedup |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 10,000 | 0.020 s | 0.020 s | 28 MiB | 0.072 s | 69 MiB | 3.5x |
| 200 | 40,000 | 0.020 s | 0.020 s | 28 MiB | 0.333 s | 136 MiB | 16x |
| 400 | 160,000 | 0.023 s | 0.024 s | 28 MiB | 4.250 s | 514 MiB | 183x |
| 800 | 640,000 | 0.032 s | 0.033 s | 29 MiB | 2.420 s | 1,382 MiB | 76x |
| 1,600 | 2,560,000 | 0.057 s | 0.058 s | 30 MiB | 11.732 s | 5,060 MiB | 206x |

O1 is ahead of Gurobi at every size. The solver path builds one binary per pair and an equality per class, so
its model grows with the square of the side; O1 sorts each side's n rows, and its memory stays flat. Gurobi's time is
not monotone in the size (160,000 pairs took longer than 640,000); that is inside its solve.
