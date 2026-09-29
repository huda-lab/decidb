# Correctness — open work

Correctness tests assert both **path selection** and **result semantics**. Tied
decision vectors need not match; schema, cardinality, feasibility, and primary
objective must. Use an independent solver oracle for representative cases so
two DeciDB paths cannot share one unnoticed mistake.

- [ ] **VAL-00 — Freeze the baseline corpus.** Depends: BASE-01. Before feature
  code, record current-syntax positive candidates and one-condition-away near
  misses, with current solver outputs, schema, and expected path. This corpus
  must not depend on the concurrent language branch.
- [ ] **VAL-01 — Harness contract tests.** Depends: HAR-01 through HAR-05.
  Exercise exact/unknown facts, complete proof objects, output binding and type
  checks, atomic miss, forced modes, and no backend/model work on a hit.
- [ ] **VAL-02 — First-rule matrix.** Depends: RULE-01 through RULE-04. Cover
  maximize/minimize; zero/one/oversized capacity; empty/nonempty input; mixed
  signs, zeros, duplicate rows, and boundary ties. Add one-condition-away
  misses for each rejected structural dimension.
- [ ] **VAL-03 — Runtime and parent-context semantics.** Depends: VAL-02,
  DES-02, DES-03. Cover NULL, NaN, infinity, deterministic throwing
  expressions, unused `x`, parent projections, filters, joins and aggregation.
  Verify validation reads exactly the relevant DECIDE input rows and no outer
  filter changes the assignment.
- [ ] **VAL-04 — Plan/serialization integration.** Depends: VAL-01, VAL-03.
  Verify logical bindings, physical output order/types, prepared statements,
  serializer round trips, and selected-rule explanation. A selected plan must
  contain no solver selection, prepared model, or `PhysicalDecide`.
- [ ] **VAL-05 — Independent and backend differential.** Depends: VAL-02 through
  VAL-04. Compare direct with forced-solver runs on Gurobi and HiGHS where
  available, and with an independent oracle on representative cases. Compare
  exact feasibility and primary objective within a justified numeric tolerance,
  not tied vectors. Run `make decide-test`, a forced-HiGHS pass, and DECIDE C++
  regressions; disclose unavailable solver coverage.

**Exit gate:** all admitted cases preserve the observable contract, all
non-admitted cases use the unchanged solver path, and serializer/prepared-plan
coverage shows the boundary is not tied to one CLI execution shape.
