# Shared Harness — open work

The harness gives proved rules a safe way to act. It must contain no S1-specific
mathematics. Read [architecture](../00_design/architecture.md), the
[first-build decisions](../00_design/decisions.md), and the
[experimental record](../00_design/experiments.md) before feature C++.

- [ ] **HAR-01 — Semantic adapter and exact facts.** Depends: first-build design. Expose a
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
- [ ] **HAR-03 — Result boundary and atomic commit.** Depends: HAR-02.
  Implement the explicit original-slot-to-child-slot map, initially retaining
  all mapped outputs and mandatory guard/rank dependencies. Lower the already
  resolved child once; verify schema, types, positional identity, and
  serializer behavior before replacing the complete DECIDE node. No miss may
  mutate the original node. Complete [DES-08](../00_design/todo.md) before
  declaring the boundary correct.
- [ ] **HAR-04 — Policy and solver fallback.** Depends: HAR-02. Add
  `off`/`auto`/`require` session control. A miss in `auto` enters the unchanged
  solver pipeline; `require` reports its reason. `DIAGNOSE` and forced solver
  bypass `auto` and conflict with `require`; prepared-plan selection follows
  the design contract. A hit never selects a backend or constructs a model.
- [ ] **HAR-05 — Structured explanation.** Depends: HAR-03. Produce
  one decision record for rule selection, proof facts, guards, and fallback.
  Attach it to the direct boundary on hits and to surviving DECIDE operators
  on misses. Surface it through optimized logical and default physical `EXPLAIN` and
  profiling without adding an algorithm-specific physical operator. Complete
  [DES-10](../00_design/todo.md) before declaring this done.
**Exit gate for the first rule:** an artificial proved proposal can replace a
DECIDE node, retain the external contract, and skip the solver; a deliberate
miss runs the original solver path. Reusability is established by
[NEXT-01](../05_follow_on/todo.md) later, not asserted from a one-rule interface.
