# Shared Harness — open work

The harness gives proved rules a safe way to act. It must contain no S1-specific
mathematics. Read [architecture](../00_design/architecture.md) and close
[DES-07](../00_design/todo.md) before feature C++.

- [ ] **HAR-01 — Semantic adapter and exact facts.** Depends: DES-07. Expose a
  read-only view of the current bound, canonical `LogicalDecide`, including
  complete objective/constraint factor membership, decision scope/domain,
  coefficient expressions, and provenance. Facts have `known`/`unknown`
  semantics; estimates cannot satisfy proof obligations. Unit-test unsupported
  constructs as ordinary unknowns.
- [ ] **HAR-02 — Rule interface and coordinator.** Depends: HAR-01. Separate
  Match, Prove, Rewrite, Map, Cost, and Explain. A proved candidate carries the
  assumptions used by its builder. The coordinator owns registration,
  selection, and typed miss reasons; one rule must not hard-code its control
  flow. Cost compares only already-proved candidates.
- [ ] **HAR-03 — Result boundary and atomic commit.** Depends: DES-01,
  DES-03, HAR-02. Implement the chosen output contract, validate schema,
  types, positional identity, required guards, and serializer behavior, then
  replace the complete DECIDE node. No recognized miss may mutate the original
  node. Verify parent projections, filters, joins, nested and correlated plans.
- [ ] **HAR-04 — Policy and solver fallback.** Depends: DES-05, HAR-02. Add
  `off`/`auto`/`require` session control. A miss in `auto` enters the unchanged
  solver pipeline; `require` reports its reason. Forced-solver and `DIAGNOSE`
  behavior follows the design contract. A hit never selects a backend or
  constructs a solver model.
- [ ] **HAR-05 — Structured explanation.** Depends: DES-04, HAR-03. Produce
  one decision record for rule selection, proof facts, guards, and fallback.
  Surface it through the agreed `EXPLAIN`/profiling path without adding an
  algorithm-specific physical operator.
**Exit gate for the first rule:** an artificial proved proposal can replace a
DECIDE node, retain the external contract, and skip the solver; a deliberate
miss runs the original solver path. Reusability is established by
[NEXT-01](../05_follow_on/todo.md) later, not asserted from a one-rule interface.
