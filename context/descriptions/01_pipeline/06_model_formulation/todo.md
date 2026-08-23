# Stage 06 — Model formulation: open work

---

## `VarIndexer::BuildRef()` has no production callers

**Pointers**: `src/include/duckdb/decidb/ilp_model.hpp:106-110`.

The non-owning constructor is retained "for tests and future use". The owning
`Build()` is used everywhere in production. Two constructors with one caller
between them is a small maintenance surface, but it is a real one: any change to
the entity-mapping lifetime has to be reasoned about twice.

**Decision needed**: delete it, or find the caller it was built for. The lifetime
contract it encodes (caller guarantees the `SolverInput` outlives the indexer) is
the harder of the two to get right, so keeping it unused is the worse of the two
outcomes.

**Test**: build + full suite; there is no behavior to pin.

**Done file**: `done.md` §1 — drop the second constructor bullet.
