# Keyed-Decision Test Coverage — Done

Tests live in `test/decide/tests/test_entity_scope.py` (keyed decisions over joins;
the file and its `test_entity_scoped_*` tests keep their names). The declaration
itself — keys of columns and relations, NULL as a class, pruning, errors — is covered
by `test_scope_declare.py` ([`../scope/done.md`](../scope/done.md)). Syntax:
`00_project_overview/syntax_reference.md` §2.3.

`decide per n: keepN(BOOL)` over `customer JOIN nation` gives one decision per nation
(`per n` keys on every column of `n`), shared by every customer row of that nation.
Oracle-verified tests compare DeciDB's output against an independently formulated
gurobipy model via `compare_solutions` or the scope oracle.

## Scenarios covered

Oracle-verified groups in `test_entity_scope.py`:

- **Core**: BOOL selection, one value per class under a tight `sum`, INT (per-row and
  aggregate bounds), REAL (single table, DOUBLE readback), keyed and per-row decisions
  in one query (`VarIndexer`'s four-block layout `[row | keyed | query-wide | auxiliary]`),
  two keyed decisions from different relations.
- **`when` / `per` composition**: a `when` in front of a constraint and of the
  objective, one- and multi-column `per` keys, `when` + `per` together.
- **Aggregate interactions**: easy and hard (Big-M) MAX/MIN, hard MIN on a keyed INT,
  keyed + `when` + hard MAX, ABS (per-row auxiliaries feeding a keyed column), AVG
  (alone and under `per`), `<>` (alone and per region via `add_ne_indicator`),
  `BETWEEN`, `=`.
- **Source shapes and NULL**: an uncorrelated scalar subquery bound shared across `per`
  classes; a three-table fan-out join with the decision keyed on the middle relation and
  `per` on an outer one; a NULL key value as one shared class
  (`test_entity_scoped_null_key`, keyed `per n.nk:`); a keyed decision beside a `per`
  constraint over the same nullable column, where NULL is one class in both
  (`test_entity_scoped_vs_per_null_semantics`, the `per` half judged by the scope
  oracle); a per-row baseline on a 1-to-many orders × lineitem join
  (`test_row_scoped_vars_on_fanout_join`).

Rows with unique caveats:

- `test_entity_scoped_mixed_when_per` — keyed + per-row + `when` + `per`,
  oracle-verified but Gurobi-only; the fixture skips on HiGHS-only hosts.
- `test_entity_scoped_nonexistent_table` — error test: `per nonexistent:` is "neither
  a column nor a relation of the FROM clause".
- `test_entity_scoped_var_in_when_condition_error` — error test: "keepN is a decision;
  when, per and by may only use data".
- `test_entity_scoped_when_entity_invisible` — error test: a `when` that keeps no row
  is "when c.c_acctbal > 9998 matches no rows" (D4).
- `test_entity_scoped_over_subquery_of_base_table` /
  `test_entity_scoped_over_cte_of_base_table` — `FROM (SELECT ... FROM base) t DECIDE
  per t: x(BOOL)` and the same shape through a `WITH` CTE keep one decision per row of
  `t`; they guard the child bindings being read before `CreatePlan` moves the projection
  expressions out (`plan_decide.cpp`).

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| keyed decision | BOOL | ✓ |
| keyed decision | INT | ✓ |
| keyed decision | REAL | ✓ |
| keyed decision | per-row decision (mixed) | ✓ |
| keyed decision | `when` (constraint) | ✓ |
| keyed decision | `when` (objective) | ✓ |
| keyed decision | `per` (one column) | ✓ |
| keyed decision | `per` (several columns) | ✓ |
| keyed decision | `when` + `per` | ✓ |
| keyed decision | MAX easy (≤ K) | ✓ |
| keyed decision | MAX hard (≥ K) | ✓ |
| keyed decision | MIN easy (≥ K) | ✓ |
| keyed decision | MIN/MAX + `when` (triple) | ✓ |
| keyed decision | ABS | ✓ |
| keyed decision | AVG (alone and under `per`) | ✓ |
| keyed decision | `<>` | ✓ |
| keyed decision | `<>` + `per` | ✓ |
| keyed decision | `BETWEEN` | ✓ |
| keyed decision | bilinear (Boolean factor) | ✓ (`test_bilinear.py::test_bilinear_entity_scoped`) |
| keyed decision | QP objective (convex) | ✓ (`test_quadratic.py::test_qp_entity_scoped_objective`) |
| keyed decision | two keyed relations | ✓ |
| keyed decision | uncorrelated scalar subquery bound + `per` (three-way) | ✓ |
| keyed decision | NULL key value (one shared class) | ✓ |
| keyed decision | three-table fan-out join + `per` on an outer relation | ✓ |
| keyed decision | source is a subquery / CTE over a base table | ✓ |
| keyed decision vs `per` | NULL key: one class in both | ✓ |
| per-row decision | 1-to-many fan-out join | ✓ |
