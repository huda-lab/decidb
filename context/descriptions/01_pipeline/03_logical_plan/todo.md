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

**Test**: a prepared statement over each metadata-bearing shape (entity scopes,
composed MIN/MAX, bilinear links, ABS maximize links, source registry) executed
twice.

**Done file**: `done.md` §5 — replace the "every new field needs a matching pair
of lines" warning with the guarantee.
