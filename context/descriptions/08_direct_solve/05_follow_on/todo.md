# Follow-on Work — open work

These items are **not** part of the first S1 vertical slice. Each promotion
requires a credible workload, exact admission proof, permanent differential
tests, and end-to-end benefit evidence. The Word catalogue owns class details.

- [ ] **NEXT-01 — Prove harness reuse with a second rule.** Depends: HAR-01
  through HAR-05 and VAL-05. Choose a different plan shape. It must use the
  existing adapter, coordinator, result contract, policy, and explanation path
  without copied control flow or S1-specific changes to shared code.
- [ ] **NEXT-03 — Wider keyed application.** Depends: VAL-05. The admitted
  row-scoped Boolean slice with source-column `PER` keys, matching top-level
  `WHEN`, aggregate-local `WHEN`, NULL-key bypass, and paired
  bounds is complete. Extend to other
  keyed shapes only after exact component independence and row/entity identity
  are proved and tested. Estimates never establish independence.
- [ ] **NEXT-04 — New-language / ANR adapter.** Depends: stable language branch,
  HAR-01, VAL-05. Map the new expression semantics into the shared problem and
  exact-fact vocabulary. Run the same adapter contract and direct/solver tests
  against both syntaxes where comparable; extend facts when semantics genuinely
  change, not by teaching rules parser spelling.
- [ ] **NEXT-05 — Cost choice among direct plans.** Depends: NEXT-01 and
  PERF-03. Introduce cost-based candidate selection only when two proved plans
  compete on real workloads. Cost cannot repair a missing proof.
- [ ] **NEXT-06 — Fixed Boolean decisions.** Depends: the admitted keyed
  slice of NEXT-03. The exact zero/one per-row pin slice now proves fixed
  selected/free row identity, adjusts each group's residual interval, retains
  every row and tested DECIDE errors, and covers `WHEN` bypass and NULL keys
  against both solvers. Source-valued row bounds such as `x<=pin` still miss;
  extend to them only when their bound and error semantics are proved from the
  complete plan. Do not infer
  fixed status from a sample.
- [ ] **NEXT-07 — Complete S1 semantic admission.** Depends: NEXT-02 and the
  admitted keyed slice of NEXT-03. Strict, fractional, negative, crossed,
  consistently foldable, and intersecting count bounds now have an exact
  proof. Numeric source-valued bounds are now admitted with solver-style
  DOUBLE group reduction, all-row NULL/NaN validation, and intersection of
  multiple dynamic limits. Deterministic, nonthrowing source-only expressions
  such as `COALESCE` and `TRY_CAST` are included. The matcher still declines
  throwing source expressions,
  offset count bodies such as `SUM(x+0)`. Independently scoped clauses such as
  quotas on two potentially overlapping subsets require a separate class,
  not a wider S1 matcher. Prove the exact solver semantics and
  error order for any form promoted. Keep unproved constraints on the solver
  path; do not add syntax cases without a semantic reason and workload benefit.
  In the current solver, a NULL source-valued bound raises even on a row whose
  `WHEN` is false or whose `PER` key is NULL; an empty scoped aggregate can
  raise before that bound error. Each wider promotion must retain that order.
  Signed additive linear objective terms are now admitted when each per-row
  coefficient is nonthrowing; a multi-term objective with a throwing
  coefficient still uses the solver path.

**Exit gate for each item:** independent proof, behavior tests, and performance
evidence; merely finding a relational expression in the catalogue is not enough.
