# Stage 06 — Model formulation: open work

---

## `VarIndexer::BuildRef()` has no production callers

**Pointers**: `VarIndexer::BuildRef` in
`src/include/duckdb/decidb/ilp_model.hpp` and
`src/decidb/utility/ilp_model_builder.cpp`.

The non-owning constructor is used throughout the focused C++ tests, while the
owning `Build()` is used in production. Two lifetime contracts are a small but
real maintenance surface: changes to entity-mapping ownership have to be reasoned
about twice.

**Decision needed**: retain and document it as a test helper, or update the tests
to use the owning constructor and delete it. Its non-owning lifetime contract
(the `SolverInput` must outlive the indexer) is the harder of the two to get right.

**Test**: build + full suite; there is no behavior to pin.

**Done file**: `done.md` §1 — drop the second constructor bullet.
