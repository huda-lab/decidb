# `per` Constraint and Two-Level Objective Test Coverage — Done

Tests live in:
- `test/decide/tests/test_per_clause.py` — `per K:` on constraints
- `test/decide/tests/test_per_multi_column.py` — keys of several columns
- `test/decide/tests/test_per_qualified.py` — relation-qualified key columns (`per s.s_nationkey:`)
- `test/decide/tests/test_per_objective.py` — the two-level objective `outer(per k: inner(e) by (k))`
- `test/decide/tests/test_per_interactions.py` — `per` composed with auxiliary-variable features (hard MIN/MAX, ABS, several decisions)

The scoping language's own suites (generation, `by`, determination, the grid) are in
[`../scope/done.md`](../scope/done.md). Syntax: `00_project_overview/syntax_reference.md`
§4, §5.2 and §6.2.

## Scenarios covered

### `per K:` on constraints

All oracle-verified:

- **Basic** (`test_per_clause.py`, `test_per_multi_column.py`): one key column with
  `per K: sum(x) by (K) <= c`, keys of two and three columns (`per a, b:` with
  `by (a, b)`), INT decisions, a `per` constraint beside an objective `when`, a `when`
  in front of a multi-column `per` (on another column and on a key column), `<>` per
  class, two `per` constraints on different columns, a `when` that leaves some classes
  without a kept row (those classes generate nothing).
- **NULL keys** (`test_per_clause.py::test_per_null_group_key`): a NULL key value is a
  class of its own; `when grp is not null per grp:` leaves those rows out. Both forms
  are judged by the scope oracle.
- **Bounds that vary within a group** (`test_per_clause.py`): with no `per` key,
  `sum(x) by (grp) <> cap` is generated once per row, so every excluded value is kept;
  under `per grp:` a bound that varies within a class is the determination error
  (`test_per_not_equal_varying_bound_under_per_key_does_not_determine`).
- **Qualified key columns** (`test_per_qualified.py`): one and several qualified
  columns, mixed qualified and bare, equivalence with the unqualified spelling, a
  table alias, a qualified column in a `when` and in the two-level objective, and an
  unknown qualifier rejected cleanly.
- **Auxiliary-variable interactions** (`test_per_interactions.py`): hard `max(e) >= K`
  and hard `min(e) <= K` per class (Big-M per group), `max(e) = K` (easy and hard
  together), `sum(abs(...))` (ABS auxiliaries per group), BOOL and INT decisions
  together, a `when` with several decisions, a QP objective beside a `per` constraint.
- **Degenerate shapes**: one-row classes, a class whose reducer has zero coefficients,
  and a NULL key under a `when` (`when active per grp:`, judged by the scope oracle) —
  all `test_per_interactions.py`; a `per` equality (two-sided bound per class,
  `test_abs_linearization.py`); a feasibility query with a `per` constraint
  (`test_edge_cases.py`); an uncorrelated scalar subquery as the bound
  (`test_cons_subquery.py`); a `BETWEEN` per class on REAL decisions
  (`test_per_clause.py::test_real_between_per_oracle`).

### Two-level objectives

`test_per_objective.py` covers `outer(per k: inner(e) by (k))` with outer ∈ {SUM, MIN,
MAX} and inner ∈ {SUM, MIN, MAX, AVG}: `sum(per k: sum(e) by (k))` equal to the flat
`sum(e)`, every `sum(per k: max/min(...))` direction (easy and hard inner),
`max/min(per k: sum(...))`, `sum(per k: avg(...))` with unequal and extremely unequal
groups, `max(per k: avg(...))` and `min(per k: avg(...))`, the two-level form under a
`when` in front, a single class, and hard inner MIN/MAX over `power(...)` (Gurobi).
`max(per g: max(c * x) by (g))` with mixed-sign coefficients is in `test_min_max.py`.

### Error cases

- An outer AVG over the classes is refused ("not supported yet") —
  `test_scope_explain_diagnose.py::test_an_outer_avg_over_the_classes_is_refused`.
- The removed spellings of a grouped constraint and of a grouped objective are
  refused by the parser (`syntax_reference.md` §10.3) — `test_scope_removed_syntax.py`.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| `per` | `when` in front | ✓ |
| `per` | a reducer's own `when` | ✓ |
| `per` | several key columns | ✓ |
| `per` | qualified key columns | ✓ |
| `per` | INT decisions | ✓ |
| `per` | NULL key values (a class of their own) | ✓ |
| `per` | `<>` | ✓ |
| `per` | MIN/MAX (easy, a bound on every member) | ✓ |
| `per` | MIN/MAX (two-level objective) | ✓ |
| `per` | AVG | ✓ |
| `per` | keyed decisions | ✓ |
| `per` | quadratic constraint | ✓ |
| `per` | quadratic constraint + `when` | ✓ |
| `per` | QP objective | ✓ |
| `per` | `when` + keyed decisions (triple) | ✓ |
| `per` | `when` + MIN/MAX (triple) | ✓ |
| `per` | `when` + AVG (triple) | ✓ |
| `per` | hard MIN/MAX constraints (Big-M per group) | ✓ |
| `per` | ABS in a reducer | ✓ |
| `per` | several decisions (BOOL + INT) | ✓ |
| `per` | `when` + several decisions | ✓ |
| `per` | equality constraint | ✓ |
| `per` | feasibility (no objective) | ✓ |
