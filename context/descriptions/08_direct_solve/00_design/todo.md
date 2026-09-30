# Design — open implementation checks

The first-build contract is in [decisions.md](decisions.md), grounded in
[experiments.md](experiments.md) and the [solver baseline](../03_correctness/baseline.md).
No unresolved design choice blocks starting feature C++. A failed check below
may change the mechanism while keeping the external semantics fixed.

- [ ] **DES-08 — Prove the production boundary under optimizer passes.**
  Depends: HAR-03, RULE-03. Audit CTE filter pusher, join order,
  unused-column removal, both column-lifetime runs, limit/TopN, late
  materialization, statistics, and join-filter pushdown on the actual DECIDE
  seam. Include parent filter, join, aggregate, nested/correlated case or an
  explicit miss, empty input, serializer, and physical output order/type tests.
  If the first retain-all map fails, repair the owning pass or use a scoped
  remap; do not retain a wrong-column plan.
- [ ] **DES-09 — Prove mandatory value reads at capacity zero and unused `x`.**
  Depends: RULE-03, RULE-04. A 5,000-row late NULL/NaN and throwing expression
  must report an error under outer `LIMIT 1`, filter, and `COUNT(*)`; an input
  `WHERE` removing the row must succeed, and outer `LIMIT 0` should avoid
  execution. Test the *actual* zero-capacity assignment; the temporary spike
  used a fixed rank threshold. If a window can be removed, use a generic
  blocking validator or let this case fall back until correct.
- [ ] **DES-10 — Check explanation and prepared-plan lifetime.** Depends:
  HAR-04, HAR-05. Show the same decision record in optimized logical and
  default physical `EXPLAIN`, profiling, and `require` errors. Prepare with
  one mode, change the setting, then execute before and after a real rebind to
  verify the stated plan-capture contract.

Selective output pruning is a performance follow-on after the retain-all
implementation is correct. Measure its cost before changing the output map;
keep guards and blocking validation live if `x` is unprojected.
