# WHEN Clause Test Coverage — Done

Tests live in:
- `test/decide/tests/test_when_constraint.py` — expression-level WHEN on constraints (11 tests)
- `test/decide/tests/test_when_perrow.py` — WHEN on per-row constraints
- `test/decide/tests/test_when_objective.py` — WHEN on objectives
- `test/decide/tests/test_when_compound.py` — compound AND/OR conditions
- `test/decide/tests/test_aggregate_local_when.py` — aggregate-local WHEN variant (39 tests)

## Scenarios covered

All oracle-verified unless noted:

- **WHEN on aggregate constraints** (`test_when_constraint.py`): string-equality and numeric-comparison conditions, constant coefficient in WHEN-gated SUM, `<>` with WHEN (expression-level), all/no rows matching (trivially applies / trivially satisfied), NULL in condition column, mixed conditional + unconditional constraints, different WHEN per constraint, constraint ordering invariance. Note: explicit `IS NOT NULL` predicate in WHEN works but requires parens around the predicate (`test_when_is_not_null_predicate`).
- **WHEN on per-row constraints** (`test_when_perrow.py`): force to zero when inactive, force selection under condition, numeric row filter, all/no rows matching. Note: REAL variable exercises the continuous skip-constraint path with no implicit [0,1] cap (`test_when_perrow_real`).
- **WHEN on objectives** (`test_when_objective.py`): MAXIMIZE + WHEN, MINIMIZE + WHEN, unconditional constraint + WHEN objective, same vs different WHEN on constraint and objective, WHEN objective matching zero rows.
- **Compound conditions** (`test_when_compound.py`): AND, OR (parenthesized), nested compound conditions.
- **Aggregate-local WHEN** (`test_aggregate_local_when.py`): independent masks on additive aggregate terms (single aggregate, three terms, overlapping filters, all rows filtered out, mixed filtered + unfiltered terms in both objective and constraint, objective atomic-comparison grammar and constraint-bound ambiguity); composition with AVG, PER, AVG + PER, easy MAX, hard MAX (`>=`, Big-M indicators restricted to WHEN-matching rows), bilinear (constraint and objective), entity-scoped, BETWEEN, `<>` (plain and + PER); regression that expression-level WHEN + PER still works.

### Error cases

`test_aggregate_local_when.py` / `test_error_binder.py`: mixing
expression-level and aggregate-local WHEN rejected; DECIDE variable in a WHEN
condition (plain and compound) rejected.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| WHEN | aggregate constraint | ✓ |
| WHEN | per-row constraint | ✓ (BOOL, INT, REAL) |
| WHEN | objective | ✓ |
| WHEN | compound AND/OR | ✓ |
| WHEN | PER | ✓ |
| WHEN | MIN/MAX (easy) | ✓ |
| WHEN | MIN/MAX (hard, aggregate-local) | ✓ |
| WHEN | AVG | ✓ |
| WHEN | ABS (objective) | ✓ |
| WHEN | QP | ✓ |
| WHEN | quadratic constraint | ✓ |
| WHEN | `<>` (expression-level) | ✓ |
| WHEN | `<>` (aggregate-local) | ✓ |
| WHEN | entity-scoped | ✓ |
| WHEN | bilinear | ✓ |
| WHEN | NULL in condition column | ✓ |
