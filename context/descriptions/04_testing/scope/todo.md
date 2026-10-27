# Scope Test Coverage — Todo

## `group_indices` drops NULL keys

`_oracle_helpers.group_indices` drops every row whose key function returns `None`, and
its docstring says that matches DECIDE's grouping. It does not: a NULL key is its own
class in `per` and its own group in `by` (D5). Nine feature files build their oracles
with it (`test_aggregate_local_when.py`, `test_avg.py`, `test_cons_correlated_subquery.py`,
`test_edge_cases.py`, `test_min_max_multiterm.py`, `test_per_clause.py`,
`test_per_multi_column.py`, `test_per_objective.py`, `test_per_qualified.py`). They pass
because their grouped keys are never NULL, so the helper is a trap for the next test
that groups on a nullable column.

- Work: keep a `None` key as its own group (as `_scope_oracle.class_ids` does), or
  replace the helper with `class_ids`; fix the docstring.
- Test: a feature test that groups on a NULL key, judged by both helpers.
- Update: `done.md` → "The reference pieces".

## Names that still say the removed spellings

Decision needed: whether to rename the test files and markers whose names carry the
removed vocabulary — `test_entity_scope.py` and its `test_entity_scoped_*` tests,
`test_scalar_scope.py`, `test_aggregate_local_when.py`, and the `per_clause` and
`when*` markers with their descriptions in `pytest.ini`. The code identifiers
they echo (`DecideVarScope::ENTITY`, `EntityScopeInfo`, the DIAGNOSE scope label
`entity`) are renamed in a later change; renaming the tests with them keeps one
vocabulary.

- Update: the README's area table and every done.md that names these files.
