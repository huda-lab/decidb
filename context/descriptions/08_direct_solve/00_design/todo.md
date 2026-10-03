# Design — open implementation checks

The first-build contract is in [decisions.md](decisions.md), grounded in
[experiments.md](experiments.md) and the [solver baseline](../03_correctness/baseline.md).
No unresolved design choice blocks starting feature C++. A failed check below
may change the mechanism while keeping the external semantics fixed.

No first-build design checks remain open. The current built-in pass audit is in
[optimizer_audit.md](optimizer_audit.md). The first selective-output pruning
check is recorded in [done.md](done.md); query-only memory evidence is in the
[benefit report](../04_performance/s1_benefit_report.md).
