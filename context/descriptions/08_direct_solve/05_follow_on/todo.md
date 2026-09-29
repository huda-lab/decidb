# Follow-on Work — open work

These items are **not** part of the first S1 vertical slice. Each promotion
requires a credible workload, exact admission proof, permanent differential
tests, and end-to-end benefit evidence. The Word catalogue owns class details.

- [ ] **NEXT-01 — Prove harness reuse with a second rule.** Depends: HAR-01
  through HAR-05 and VAL-05. Choose a different plan shape. It must use the
  existing adapter, coordinator, result contract, policy, and explanation path
  without copied control flow or S1-specific changes to shared code.
- [ ] **NEXT-02 — Wider cardinality outcomes.** Depends: VAL-05. Add exact and
  lower bounds only after relational infeasibility and empty-input outcomes are
  proved, implemented, and tested.
- [ ] **NEXT-03 — Keyed application.** Depends: VAL-05. Extend only after exact
  component independence, row/entity identity, and NULL `PER` semantics are
  proved and tested. Estimates never establish independence.
- [ ] **NEXT-04 — New-language / ANR adapter.** Depends: stable language branch,
  HAR-01, VAL-05. Map the new expression semantics into the shared problem and
  exact-fact vocabulary. Run the same adapter contract and direct/solver tests
  against both syntaxes where comparable; extend facts when semantics genuinely
  change, not by teaching rules parser spelling.
- [ ] **NEXT-05 — Cost choice among direct plans.** Depends: NEXT-01 and
  PERF-03. Introduce cost-based candidate selection only when two proved plans
  compete on real workloads. Cost cannot repair a missing proof.

**Exit gate for each item:** independent proof, behavior tests, and performance
evidence; merely finding a relational expression in the catalogue is not enough.
