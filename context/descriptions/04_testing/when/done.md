# `when` Test Coverage — Done

Tests live in:
- `test/decide/tests/test_when_constraint.py` — `when c:` in front of an aggregate constraint
- `test/decide/tests/test_when_perrow.py` — `when c:` in front of a per-row constraint
- `test/decide/tests/test_when_objective.py` — `MAXIMIZE|MINIMIZE when c: E`
- `test/decide/tests/test_when_compound.py` — compound AND/OR conditions
- `test/decide/tests/test_when_grammar.py` — parenthesized and bare conditions
- `test/decide/tests/test_aggregate_local_when.py` — a reducer's own `when`, `agg(when c: e)`

The scoping language's own suites (kept rows, NULL conditions, the empty-set errors,
the grid) are in [`../scope/done.md`](../scope/done.md). Syntax:
`00_project_overview/syntax_reference.md` §4, §5.3, §6.3 and §7.2.

## Scenarios covered

All oracle-verified unless noted:

- **`when` in front of an aggregate constraint** (`test_when_constraint.py`):
  string-equality and numeric conditions, a constant coefficient in the reducer, `<>`
  in the condition, every row kept (the same as no `when`), NULL in the condition column
  (the row is dropped), an explicit `IS NOT NULL` condition, conditional and
  unconditional constraints together, a different `when` per constraint, constraint
  order invariance. A condition holding a subquery under `COALESCE`, `IS NULL`, `NOT` or
  `BETWEEN` is quoted as written in the "matches no rows" and "has no rows" messages, in a
  constraint, a reducer and the objective, with no internal name; a DIAGNOSE suggestion
  over a column named `per` quotes it and pastes back
  (`test_a_condition_holding_a_subquery_is_quoted_as_written`,
  `test_a_condition_on_a_column_named_per_pastes_back`).
- **`when` in front of a per-row constraint** (`test_when_perrow.py`): force to zero,
  force selection, a numeric row filter, every row kept, and a REAL decision with no
  implicit [0, 1] cap (`test_when_perrow_real`).
- **`when` on the objective** (`test_when_objective.py`): MAXIMIZE and MINIMIZE under a
  `when`, an unconditional constraint beside it, the same and different conditions on
  the constraint and the objective, every row kept.
- **Compound conditions** (`test_when_compound.py`): AND and OR, nested, mixed string
  and numeric, on aggregate and per-row constraints and on the objective.
- **Condition grammar** (`test_when_grammar.py`): parenthesized `NOT`, comparison and
  arithmetic conditions on a reducer's `when` and on the objective's; a bare comparison
  after the objective's `when` gives the same answer as its parenthesized spelling. A
  condition runs to its colon, so no condition needs parentheses.
- **A reducer's own `when`** (`test_aggregate_local_when.py`): independent filters on
  additive reducers (one reducer, three, overlapping filters, filtered beside
  unfiltered, in the constraint and the objective); a `when` in front applied before a
  reducer's own; composition with AVG, `per`, AVG + `per`, easy MAX, hard MAX (`>=`,
  Big-M indicators only on kept rows), bilinear terms (constraint and objective), keyed
  decisions, `BETWEEN`, `<>` (plain and under `per`); constant offsets and scalar
  multipliers around a filtered reducer; a data column multiplying or dividing a
  filtered reducer (`col * sum(when w: x) <= 20`), which takes the value shape and is
  judged by the scope oracle.

### Empty row sets

A `when` in front that keeps no row is an error, "when c matches no rows." (D4):
`test_when_constraint.py::test_when_no_rows_match`,
`test_when_perrow.py::test_when_perrow_no_matches`,
`test_when_objective.py::test_when_objective_no_match`. A reducer whose own `when`
keeps no row is an error, "agg(when c: e) has no rows." (D8):
`test_aggregate_local_when.py::test_aggregate_local_when_all_filtered_out` and the
mixed-filter cases, and the empty MIN/MAX, SUM and AVG cases of `test_edge_cases.py`
(see [`../min_max/done.md`](../min_max/done.md)). A `when` in front that leaves one
`per` class without a kept row generates nothing for that class
(`test_per_multi_column.py::test_multi_column_per_when_eliminates_all_in_group`).

### Error cases

A decision in a condition is refused with "x is a decision; when, per and by may only
use data" — `test_error_binder.py` (plain and compound) and
`test_aggregate_local_when.py` (a reducer's `when` and the objective's). The removed
spellings of a condition are refused by the parser (`syntax_reference.md` §10.3) —
`test_scope_removed_syntax.py`.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| `when` | aggregate constraint | ✓ |
| `when` | per-row constraint | ✓ (BOOL, INT, REAL) |
| `when` | objective | ✓ |
| `when` | compound AND/OR | ✓ |
| `when` | `per` | ✓ |
| `when` | MIN/MAX (easy) | ✓ |
| `when` | MIN/MAX (hard, a reducer's own `when`) | ✓ |
| `when` | AVG | ✓ |
| `when` | ABS (objective) | ✓ |
| `when` | QP | ✓ |
| `when` | quadratic constraint | ✓ |
| `when` | `<>` comparison under a `when` in front | ✓ (`test_cons_comparison.py`) |
| `when` | `<>` (a reducer's own) | ✓ |
| `when` | keyed decisions | ✓ |
| `when` | bilinear | ✓ |
| `when` | NULL in condition column | ✓ |
| `when` | a data column scaling a filtered reducer | ✓ (scope oracle) |
