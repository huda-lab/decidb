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

## Hand-checked scope tests

The README forbids hand-computed answers, yet some scope tests assert a small answer
their docstring derives, mostly because the scope oracle cannot state the shape (a
square, an `abs` around a helper, a product beside a reducer, an `AVG` under `<>`):
`test_scope_reducer_by.py`
(`test_avg_not_equal`, `test_a_square_in_a_grouped_reducer`),
`test_scope_value_rewrite.py` (`test_two_keys_hand_checked`,
`test_a_query_wide_decision_beside_a_reducer_in_a_product_row`,
`test_a_reducer_inside_a_square`, `test_a_square_of_a_helper_with_an_inner_when`,
`test_abs_of_a_helper_bounds_the_total`, `test_a_helper_inside_a_hard_abs_is_boxed_with_it`),
`test_scope_generation.py` (`test_a_fractional_strict_bound_steps_onto_the_lattice`,
`test_rows_a_when_drops_are_never_read`, `test_a_norm_reads_only_the_rows_its_when_keeps`,
`test_a_when_guards_a_function_that_fails_on_its_dropped_rows`) and
`test_scope_objective.py` (`test_when_in_front_reads_kept_rows_only`).

- Work: judge each by `_brute_force` (small whole-number domains) or by `ScopedModel`
  where it can state the shape.
- Update: `done.md` → "The suites" and "Coverage tiers".

## Names that still say the removed spellings

Decision needed: whether to rename the test files and markers whose names carry the
removed vocabulary — `test_entity_scope.py` and its `test_entity_scoped_*` tests,
`test_scalar_scope.py`, `test_aggregate_local_when.py`, and the `per_clause` and
`when*` markers with their descriptions in `pytest.ini`. The code identifiers
they echo (`DecideVarScope::ENTITY`, `EntityScopeInfo`, the DIAGNOSE scope label
`entity`) are renamed in a later change; renaming the tests with them keeps one
vocabulary.

- Update: the README's area table and every done.md that names these files.
