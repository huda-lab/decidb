# Input and coefficient extraction

DuckDB operator profiles expose scans, joins, sorting and output-table writing. Per-chunk accumulators measure sink append and solution-to-row mapping without logging every chunk. Sink-combine timing includes its lock wait.

Finalize spans separate entity mapping, constraint evaluation, objective evaluation and composed MIN/MAX evaluation. Constraint subspans isolate setup, linear coefficients, WHEN/PER/qualifiers, RHS and quadratic coefficients. Nested group-id and Boolean-mask spans identify shared helpers; constraint ordinals identify clauses.

