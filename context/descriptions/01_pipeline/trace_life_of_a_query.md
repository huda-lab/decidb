# Life of a query: the knapsack trace

One characteristic decision query, followed through all eight stages. Every output
below was captured from `build/release/decidb` on 2026-08-14, not reconstructed.

## The query

```sql
CREATE TABLE Items(id INT, value INT, weight INT, category VARCHAR);
INSERT INTO Items VALUES
  (1,100,20,'electronics'),
  (2, 60,10,'electronics'),
  (3,120,30,'electronics'),
  (4, 50,50,'furniture');

SELECT id, value, weight, x
FROM Items
WHERE category = 'electronics'
DECIDE x(BOOL)
SUCH THAT SUM(x * weight) <= 50
MAXIMIZE SUM(x * value);
```

---

## Stage 01 — Parser

The lexer sets `in_decide_clause` on the `DECIDE` token, so any `WHEN` after it
would lex as `WHEN_DECIDE`; this query has none. `decide_clause` matches
`DECIDE typed_decide_variable_list opt_decide_tail`, and `x(BOOL)` matches the
`ColId '(' variable_type ')'` alternative — the row-scoped form.

The transformer moves three things onto the `SelectNode`: `decide_variables`
(one `PG_AEXPR_OF` node pairing `x` with `bool_variable`), `decide_constraints`,
and `decide_objective`, with `decide_sense = MAXIMIZE`.

Parsed-tree rewrites: nothing fires. No qualifier to strip, no cast over a
decision, no `norm()`, and no `IN`. The constraint reaches the binder exactly
as written.

## Stage 02 — Binder

`x` does not collide with a column and is not a duplicate, so it registers at
index 0. No table qualifier, so `variable_scopes[0] = Row()`.
`var_types[0] = LogicalType::INTEGER` and `is_boolean_var[0] = true`.

**No `x >= 0 AND x <= 1` constraints are synthesized.** The `[0,1]` box travels as
`is_boolean_var` and is applied directly to the solver column at model-build time.

`x * weight` binds: degree 1 in the decision variables, since `weight` is data.
`WHERE category='electronics'` binds as an ordinary filter, entirely outside the
DECIDE machinery.

## Stage 03 — Logical plan

No scalar subqueries, so `PlanSubqueries` does nothing and both provenance sets
are empty. `LogicalDecide` is built above the filter:

```
LogicalDecide  [x]
 ├── Constraints: SUM(x*weight) <= 50
 ├── Objective:   MAXIMIZE SUM(x*value)
 └── LogicalFilter (category = 'electronics')
      └── LogicalGet (Items)
```

## Stage 04 — Canonicalizer

The constraint is already canonical, and the pass confirms it rather than changing
it: `SUM(x*weight)` is one decision-bearing atom on the left, `50` is one
decision-free atom on the right, no scale to peel, no cast to see through. The
classification is `AGGREGATE` — one reducer-rooted decision term, no row-varying
algebra beside it.

The objective is canonicalized through the same machinery: one additive term,
decision-bearing, nothing to fold into `objective_constant_offset`.

`VerifyCanonical` and `VerifyCanonicalObjective` both pass, and re-canonicalizing
a copy produces an identical tree — the C7 / O5 fixed point.

## Stage 05 — Optimizer

All seven passes run and none of them changes anything: no `ABS`, no bilinear
product, no `MIN`/`MAX`, no `<>`, no `AVG`. `LogicalDecide` comes out of
`OptimizeDecide` as it went in.

Verification runs once more at physical-plan entry.

## Stage 08 — Extraction, then materialization

The sink state is constructed **before** the first row arrives.
`TraverseBoundsConstraints` finds nothing to absorb — the only constraint has a
reducer on its left, so it is not a bound. Flattening (`AnalyzeConstraint`) produces one
`DecideConstraint` with `lhs_is_aggregate = true` and one `Term`
`{variable_index: 0, coefficient: weight, sign: +1}`. `AnalyzeObjective` produces
one term with coefficient `value`.

Then `Sink` runs. Row 4 was already eliminated by the filter, so three rows are
buffered:

| RowIdx | id | value | weight |
|---:|---:|---:|---:|
| 0 | 1 | 100 | 20 |
| 1 | 2 | 60 | 10 |
| 2 | 3 | 120 | 30 |

## Stage 08 — Finalize

**PHASE 1.5** — no entity scopes, so no mappings.

**PHASE 2** — the coefficient expression `weight` is transformed from a
`BoundColumnRefExpression` to a `BoundReferenceExpression` on the chunk position,
executed over the buffer, and cast once to `double`:

- constraint: `row_coefficients[0] = [20, 10, 30]`, `rhs_values = [50]`
- objective: `[100, 60, 120]`

No `WHEN`, no `PER`, so `row_group_ids` stays empty and `num_groups` is 0 — the
fast path. Nothing is NULL, NaN or infinite.

## Stage 06 — Model formulation

One row-scoped variable, three rows, no entities, no scalars, no globals: the
`VarIndexer` layout is just block 1, three columns.
`CanUseRowScopedFastPath` returns true — every term is row-scoped and `x` appears
once — so coefficients push directly into COO.

`variable_types[0]` is reported as `BOOLEAN` (because `is_boolean_var`), giving
`[0,1]` and `is_binary`.

```
Maximize
 obj: 100 x0 + 60 x1 + 120 x2
Subject To
 c1: 20 x0 + 10 x1 + 30 x2 <= 50
Bounds
 0 <= x0 <= 1 ; 0 <= x1 <= 1 ; 0 <= x2 <= 1
Binaries
 x0 x1 x2
```

## Stage 07 — Solver

`SelectSolverBackend()` returns Gurobi if available, HiGHS otherwise. Either way
the result normalizes to `SolverStatus::OPTIMAL` with
`solution = [1, 0, 1]` and `objective_value = 220`.

Check: `20·1 + 10·0 + 30·1 = 50 ≤ 50`, value `100 + 120 = 220`. Taking rows 1 and 2
instead would weigh 30 and be worth only 160.

## Stage 08 — Readback

`GetData` re-scans the buffer and appends `x`. The variable's output type is
`INTEGER` — `BOOL` is a domain, not a storage type — so the projection is
`round(value)` and the column comes back as `int32`:

```
┌───────┬───────┬────────┬───────┐
│  id   │ value │ weight │   x   │
│ int32 │ int32 │ int32  │ int32 │
├───────┼───────┼────────┼───────┤
│     1 │   100 │     20 │     1 │
│     2 │    60 │     10 │     0 │
│     3 │   120 │     30 │     1 │
└───────┴───────┴────────┴───────┘
```

---

## What this trace does not exercise

Deliberately — it is the simplest query that is still a real decision problem.
For the machinery it skips, follow the stage docs:

| Not exercised | Where to read |
|---|---|
| Side migration, relation flipping, scale peeling | [`04_canonicalizer/done.md`](04_canonicalizer/done.md) §3.4–3.7 |
| Any linearization (ABS, MIN/MAX, `<>`, McCormick) | [`05_optimizer/done.md`](05_optimizer/done.md) §2 |
| `WHEN` masks and `PER` grouping | [`08_execution/done.md`](08_execution/done.md) §5 |
| Entity-scoped variables | [`08_execution/done.md`](08_execution/done.md) §4 |
| Subquery bounds and their provenance | [`03_logical_plan/done.md`](03_logical_plan/done.md) §2 |
| The accumulation path in model building | [`06_model_formulation/done.md`](06_model_formulation/done.md) §4 |
