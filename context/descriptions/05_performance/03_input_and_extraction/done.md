# Input and coefficient extraction

DuckDB operator profiles expose scans, joins, sorting and output-table writing. Per-chunk accumulators measure sink append and solution-to-row mapping without logging every chunk. Sink-combine timing includes its lock wait.

Finalize spans separate the keyed-decision mapping (`execution.entity_mapping`), the run-time scope checks (`execution.scope_checks`), constraint evaluation, objective evaluation, composed MIN/MAX evaluation and the helper decisions' members (`execution.evaluate_value_links`). Constraint subspans isolate setup, linear coefficients, the `when` / `per` / `by` grouping with a reducer's inner `per` and colon form (`extraction.when_per_and_qualifiers`), RHS and quadratic coefficients. Nested group-id and Boolean-mask spans identify shared helpers; constraint ordinals identify clauses.

