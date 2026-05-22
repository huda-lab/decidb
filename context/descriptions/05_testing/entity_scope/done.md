# Entity-Scope Test Coverage — Done

Tests live in `test/decide/tests/test_entity_scope.py`.

Tests marked ✓ compare DecidB's output against an independently-formulated
gurobipy ILP via `compare_solutions` (objective + decision vector). Tests
marked (constraint only) verify feasibility but not optimality — a legacy
tier per `05_testing/README.md`; new tests in this area should move to
oracle-verified.

| Test | What it covers | Oracle |
|------|---------------|--------|
| `test_entity_scoped_nation_selection` | Basic BOOLEAN, MAXIMIZE SUM(keepN * acctbal), SUM constraint | ✓ |
| `test_entity_scoped_consistency` | Tight SUM <= 5, entity consistency under pressure | ✓ |
| `test_entity_scoped_integer` | IS INTEGER variable, per-row bound + aggregate bound | ✓ |
| `test_entity_scoped_mixed_with_row_scoped` | VarIndexer three-block layout: entity + row-scoped coexist | ✓ |
| `test_entity_scoped_with_when` | WHEN condition on constraint, entity-scoped | ✓ |
| `test_entity_scoped_nonexistent_table` | Error: scoping to nonexistent table | — (error test) |
| `test_entity_scoped_with_per` | PER grouping + entity-scoped, per-region oracle | ✓ |
| `test_entity_scoped_with_max` | MAX(expr) <= K easy case, MIN/MAX linearization | ✓ |
| `test_entity_scoped_with_avg` | AVG → SUM scaling with entity-scoped | ✓ |
| `test_entity_scoped_when_per_triple` | WHEN + PER + entity-scoped triple interaction | ✓ |
| `test_entity_scoped_ne_constraint` | NE (`<>`) Big-M rewrite + entity-scoped | constraint only |
| `test_entity_scoped_max_hard_case` | MAX >= K hard case (Big-M) + entity-scoped | ✓ |
| `test_entity_scoped_mixed_when_per` | All-four: entity + row-scoped + WHEN + PER | ✓ (Gurobi-only; fixture skips on HiGHS-only hosts) |
| `test_entity_scoped_when_on_objective` | WHEN on objective + entity-scoped | ✓ |
| `test_entity_scoped_multi_column_per` | Multi-column PER (region × segment) + entity-scoped | ✓ |
| `test_entity_scoped_min_easy_case` | MIN >= K easy case + entity-scoped | ✓ |
| `test_entity_scoped_avg_per` | AVG + PER + entity-scoped | ✓ |
| `test_entity_scoped_ne_per` | NE + PER + entity-scoped, oracle via `add_ne_indicator` per region | ✓ |
| `test_entity_scoped_between_constraint` | BETWEEN constraint + entity-scoped | constraint only |
| `test_entity_scoped_two_tables` | Two entity-scoped vars from different tables (n + r) | ✓ |
| `test_entity_scoped_var_in_when_condition_error` | Error: DECIDE var in WHEN condition | — (error test) |
| `test_entity_scoped_when_entity_invisible` | WHEN filters all rows for some entities | ✓ |
| `test_entity_scoped_equality_constraint` | Equality (=) constraint + entity-scoped | constraint only |
| `test_entity_scoped_is_real` | IS REAL entity-scoped, single-table, DOUBLE readback | ✓ |
| `test_entity_scoped_hard_min_max` | MIN <= K hard case + entity-scoped INTEGER | ✓ |
| `test_entity_scoped_abs` | ABS linearization + entity-scoped (per-row aux → entity) | ✓ |
| `test_entity_scoped_when_min_max_triple` | Entity-scoped + WHEN + hard MAX (triple interaction) | ✓ |
| `test_entity_scoped_ne_oracle` | Entity-scoped + `<>` (NE) Big-M with objective verification | ✓ |
| `test_row_scoped_vars_on_fanout_join` | Row-scoped (non-entity) vars on 1-to-many orders×lineitem JOIN — baseline contrast | ✓ |
| `test_entity_scoped_subquery_per_three_way` | Scalar uncorrelated subquery RHS + PER + entity-scoped (three-way) | ✓ |
| `test_entity_scoped_null_key` | NULL in entity-key column groups into a single shared entity | ✓ |
| `test_entity_scoped_three_way_join_per_region` | customer × nation × region fan-out, entity variable on nation, PER on outer-table column | ✓ |
| `test_entity_scoped_over_subquery_of_base_table` | Regression: `FROM (SELECT ... FROM base) t DECIDE t.x ...` — used to silently collapse to one entity because child bindings were read after CreatePlan moved projection expressions out (`plan_decide.cpp`) | ✓ |
| `test_entity_scoped_over_cte_of_base_table` | Same regression shape via WITH-CTE | ✓ |
| `test_entity_scoped_vs_per_null_semantics` | Side-by-side divergence: entity-scope collapses NULL keys into one shared entity; PER excludes NULL-keyed rows from groups (rows float free of the cap) | ✓ |

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
