# Testing — open work

Open coverage gaps, one section each. Coverage that exists is in [coverage.md](coverage.md) and
[scope_suite.md](scope_suite.md).

## BLOAT-13 — Bloat in the scope tests

- **Problem.** These suites (about 16,285 lines in 28 files) grew by cross-product grids and have not been read for
  bloat: `test_scope_*`, `test_per_*`, `test_keyed_scope`, `test_global_scope`,
  `test_qualified_*`, `test_decision_name_scope`, `test_message_qualifier`, `test_collated_keys`, `test_null_keys` and
  `test_nested_decide`.
- **Change.** Remove the bloat by `../direct_solve/framework/todo.md` § How to work any task (Removing bloat): keep one
  case per branch it reaches, named for it, and keep every kept test's name (the oracle cache keys on it). Shared
  helpers in `test/decide/tests/_*.py` change only when every caller is in these files.
- **Done when.** `make decide-test` passes, every remaining case names the branch it reaches, and the report lists the
  removals with the case that already reaches each branch, and the case count and lines before and after.


## BLOAT-14 — Bloat in the constraint tests

- **Problem.** These suites (about 16,052 lines in 42 files) grew by cross-product grids and have not been read for
  bloat: `test_cons_*`, `test_canonicalize_*`, `test_when_*`, `test_var_*`,
  `test_clause_order`, `test_like_term_collection`, `test_ne_range_collapse`, `test_implied_bounds`,
  `test_infinite_bounds`, `test_auxiliary_bounds`, `test_bound_side_tables`, `test_bool_bound_outside_domain`,
  `test_reduced_bound_data_column`, `test_rhs_aggregate_window`, `test_coefficient_window`, `test_data_operands`,
  `test_singleton_row`, `test_signed_variables`, `test_empty_groups` and `test_int_result_range`.
- **Change.** Remove the bloat by `../direct_solve/framework/todo.md` § How to work any task (Removing bloat): keep one
  case per branch it reaches, named for it, and keep every kept test's name (the oracle cache keys on it). Shared
  helpers in `test/decide/tests/_*.py` change only when every caller is in these files.
- **Done when.** `make decide-test` passes, every remaining case names the branch it reaches, and the report lists the
  removals with the case that already reaches each branch, and the case count and lines before and after.


## BLOAT-15 — Bloat in the nonlinear and reducer tests

- **Problem.** These suites (about 17,327 lines in 21 files) grew by cross-product grids and have not been read for
  bloat: `test_quadratic*`, `test_bilinear`, `test_abs_linearization`,
  `test_min_max*`, `test_minmax_*`, `test_norm`, `test_power_over_data`, `test_product_normal_form`,
  `test_nested_product_in_reducer`, `test_obj_*`, `test_avg`, `test_null_coalesce_reducer`, `test_reducer_filter`,
  `test_scalar_var_in_aggregate`, `test_oracle_api_quadratic`, `test_large_number_warning` and `test_unbounded_bigm`.
- **Change.** Remove the bloat by `../direct_solve/framework/todo.md` § How to work any task (Removing bloat): keep one
  case per branch it reaches, named for it, and keep every kept test's name (the oracle cache keys on it). Shared
  helpers in `test/decide/tests/_*.py` change only when every caller is in these files.
- **Done when.** `make decide-test` passes, every remaining case names the branch it reaches, and the report lists the
  removals with the case that already reaches each branch, and the case count and lines before and after.


## BLOAT-16 — Bloat in the error, output and end-to-end tests

- **Problem.** These suites (about 6,511 lines in 20 files) grew by cross-product grids and have not been read for
  bloat: `test_error_*`, `test_edge_cases`, `test_explain`,
  `test_invalid_value_message`, `test_native_constructs`, `test_null_values`, `test_coverage_tooling`,
  `test_large_scale`, `test_sql_joins`, `test_pipeline_profile`, `test_written_text`,
  `test_parser_catalog_introspection`, `test_column_spelling`, `test_no_old_spellings_remain` and `test_mip_gap`.
- **Change.** Remove the bloat by `../direct_solve/framework/todo.md` § How to work any task (Removing bloat): keep one
  case per branch it reaches, named for it, and keep every kept test's name (the oracle cache keys on it). Shared
  helpers in `test/decide/tests/_*.py` change only when every caller is in these files.
- **Done when.** `make decide-test` passes, every remaining case names the branch it reaches, and the report lists the
  removals with the case that already reaches each branch, and the case count and lines before and after.
