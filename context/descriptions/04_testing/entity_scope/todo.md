# Entity-Scope Test Coverage — Todo

No open gaps.

The previously listed NULL-semantics divergence is now covered by
`test_entity_scope.py::test_entity_scoped_vs_per_null_semantics`, which runs
the same dataset through both an entity-scope query and a row-scope+PER query
and oracle-verifies each side independently, asserting the two optima differ
in the documented direction (entity-scope shares one variable across NULL
rows; PER excludes NULL-keyed rows from groups).
