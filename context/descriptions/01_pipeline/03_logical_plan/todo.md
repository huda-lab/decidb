# Stage 03 — Logical plan: open work

---

## Hand-maintained serialization has no test that a new field was added

**Pointers**: `LogicalDecide::Serialize` and `LogicalDecide::Deserialize` in
`src/planner/operator/logical_decide.cpp`.

`LogicalDecide` is marked `"custom_implementation": true`, so DuckDB's generated
serializer is bypassed and both directions are written by hand. Dozens of
properties and several structs are flattened into parallel vectors. Adding a field to
the header and forgetting the two serializer lines compiles, passes every test
that does not use a prepared statement, and silently drops the field on replay.

The same hazard has a documented sibling one layer down: the
`ColumnBindingResolver` enumeration in `src/execution/column_binding_resolver.cpp`
must also be extended for any new expression-holding field, and forgetting *that*
is invisible on a single-table source.

**Decision needed**: whether this is worth a round-trip test
(serialize → deserialize → compare field by field) or whether the two comments
already in place are sufficient. A round-trip test would need a comparison that
fails on a *missing* field rather than comparing only the fields it knows about —
otherwise it reproduces the bug it is meant to catch.

**Considered and rejected (2026-08-29)**: a field-by-field round-trip test does not
close this. Adding a field means remembering to add it to the test as well, which is
the same act of remembering the bug depends on — the guard fails exactly when the code
does. Comparing serialized bytes across a round trip is no better: a dropped field is
absent from both sides and the comparison passes. There is no cheap assertion that
fails on its own for a field nobody wrote code for.

What would actually close it is removing the hand-written serializer: drop
`"custom_implementation": true` and let DuckDB generate both directions, so a new field
is covered by construction. That needs every struct currently flattened into parallel
vectors (entity scopes, bilinear links, ABS maximize links, the source registry) to
become serializable in its own right. That is the real fix and it is a project, not a
test.

Until then the mitigation is per-field and deliberate: any new field ships with a test
that exercises it through a prepared statement. `source_column_names` and its two index
vectors (property ids 246-248, added 2026-08-29 for unbounded escape characterization)
were the first done this way — see
`test_query_diagnostics_escaping_instances.py::test_column_names_survive_a_prepared_statement`.
That is a genuine test of behavior rather than a guard needing maintenance, but it
scales by discipline, not by construction, which is why this item stays open.

**Test**: a prepared statement over each metadata-bearing shape (entity scopes,
composed MIN/MAX, bilinear links, ABS maximize links, source registry) executed
twice.

**Done file**: `done.md` §5 — replace the "every new field needs a matching pair
of lines" warning with the guarantee.
