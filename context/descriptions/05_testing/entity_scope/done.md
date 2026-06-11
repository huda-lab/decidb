# Entity-Scope Test Coverage — Done

Tests live in `test/decide/tests/test_entity_scope.py` (36 tests).

Oracle-verified tests compare DeciDB's output against an independently-formulated
gurobipy ILP via `compare_solutions` (objective + decision vector). A few tests
are constraint-only (verify feasibility but not optimality) — a legacy tier per
`05_testing/README.md`; new tests in this area should move to oracle-verified.

## Scenarios covered

Oracle-verified groups in `test_entity_scope.py`:

- **Core**: basic BOOLEAN selection, tight-SUM entity consistency under pressure, IS INTEGER (per-row + aggregate bounds), IS REAL (single-table, DOUBLE readback), mixed entity- + row-scoped variables (VarIndexer three-block layout), two entity-scoped vars from different tables.
- **WHEN / PER composition**: WHEN on constraint and on objective, single- and multi-column PER, WHEN + PER triple, WHEN filtering all rows for some entities.
- **Aggregate interactions**: MAX/MIN easy and hard (Big-M) cases, hard MIN on entity-scoped INTEGER, entity + WHEN + hard MAX triple, ABS linearization (per-row aux → entity), AVG → SUM scaling (standalone and + PER), NE (`<>`) Big-M with objective verification, NE + PER (oracle via `add_ne_indicator` per region).
- **Source shapes / NULL semantics**: scalar uncorrelated subquery RHS + PER + entity-scoped (three-way), three-table fan-out JOIN with entity variable on the inner table and PER on an outer-table column, NULL entity-key column grouping into a single shared entity, side-by-side entity-scope vs PER NULL-key divergence (entity-scope collapses NULL keys into one shared entity; PER excludes NULL-keyed rows from groups — rows float free of the cap), and a row-scoped baseline contrast on a 1-to-many orders×lineitem JOIN.

Rows with unique caveats:

- `test_entity_scoped_ne_constraint`, `test_entity_scoped_between_constraint`, `test_entity_scoped_equality_constraint` — constraint only (legacy tier).
- `test_entity_scoped_mixed_when_per` — all-four interaction (entity + row-scoped + WHEN + PER), oracle-verified but Gurobi-only; fixture skips on HiGHS-only hosts.
- `test_entity_scoped_nonexistent_table`, `test_entity_scoped_var_in_when_condition_error` — error tests (no oracle).
- `test_entity_scoped_over_subquery_of_base_table` / `test_entity_scoped_over_cte_of_base_table` — regression: `FROM (SELECT ... FROM base) t DECIDE t.x ...` used to silently collapse to one entity because child bindings were read after CreatePlan moved projection expressions out (`plan_decide.cpp`); same shape via WITH-CTE.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| entity_scope | BOOLEAN | ✓ |
| entity_scope | INTEGER | ✓ |
| entity_scope | REAL | ✓ |
| entity_scope | row-scoped (mixed) | ✓ |
| entity_scope | WHEN (constraint) | ✓ |
| entity_scope | WHEN (objective) | ✓ |
| entity_scope | PER (single-column) | ✓ |
| entity_scope | PER (multi-column) | ✓ |
| entity_scope | WHEN + PER | ✓ |
| entity_scope | MAX easy (≤ K) | ✓ |
| entity_scope | MAX hard (≥ K) | ✓ |
| entity_scope | MIN easy (≥ K) | ✓ |
| entity_scope | MIN/MAX + WHEN (triple) | ✓ |
| entity_scope | ABS | ✓ |
| entity_scope | AVG (standalone and + PER) | ✓ |
| entity_scope | NE (`<>`) | ✓ |
| entity_scope | NE + PER | ✓ |
| entity_scope | BETWEEN | ✓ |
| entity_scope | bilinear (Boolean factor) | ✓ (`test_bilinear.py::test_bilinear_entity_scoped`) |
| entity_scope | QP objective (convex) | ✓ (`test_quadratic.py::test_qp_entity_scoped_objective`) |
| entity_scope | two entity-scoped tables | ✓ |
| entity_scope | WHEN filters entity to zero | ✓ |
| entity_scope | uncorrelated scalar subquery RHS + PER (three-way) | ✓ |
| entity_scope | NULL in entity-key column (single shared entity) | ✓ |
| entity_scope | three-table fan-out JOIN + PER on outer table | ✓ |
| entity_scope | source is subquery / CTE wrapping a base table | ✓ (regression for `plan_decide.cpp` child-bindings-after-CreatePlan bug) |
| entity_scope vs PER | NULL-key semantics divergence (shared entity vs group exclusion) | ✓ |
| row-scoped (not entity-scoped) | 1-to-many fan-out JOIN | ✓ |
