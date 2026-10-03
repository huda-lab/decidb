# Shared Harness — open work

The harness gives proved rules a safe way to act. It must contain no S1-specific
mathematics. Read [architecture](../00_design/architecture.md), the
[first-build decisions](../00_design/decisions.md), and the
[experimental record](../00_design/experiments.md) when extending the prototype.

- [ ] **HAR-02 — Rule interface and coordinator.** Depends: HAR-01. Separate
  Match, Prove, Rewrite, Map, Cost, and Explain. The first interface and
  coordinator are implemented; prove reuse by registering a second, different
  rule without copied policy, fallback, or output-map logic. Cost must compare
  only already-proved candidates and must not silently prefer an unsupported
  rule.
**Exit gate for the first rule:** the exact-fact adapter in [HAR-01](done.md)
feeds a proved S1 proposal that replaces a DECIDE node,
retains the external contract, and skips the solver; a deliberate miss runs
the original solver path. That boundary is covered in
[HAR-03](done.md) and the [optimizer audit](../00_design/optimizer_audit.md).
Reusability is established by [NEXT-01](../05_follow_on/todo.md) later, not
asserted from a one-rule interface.
