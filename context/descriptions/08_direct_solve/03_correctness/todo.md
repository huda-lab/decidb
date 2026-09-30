# Correctness — open work

Correctness tests assert both **path selection** and **result semantics**. Tied
decision vectors need not match; schema, cardinality, feasibility, and primary
objective must. Use an independent solver oracle for representative cases so
two DeciDB paths cannot share one unnoticed mistake.

- [ ] **VAL-01 — Harness contract tests.** Depends: HAR-01 through HAR-05.
  Exercise exact/unknown facts, complete proof objects, output binding and type
  checks, atomic miss, forced modes, and no backend/model work on a hit. Promote
  the [baseline](baseline.md) to executable tests with explicit path markers.
- [ ] **VAL-02 — First-rule matrix.** Depends: RULE-01 through RULE-04. Cover
  maximize/minimize; zero/one/oversized capacity; empty/nonempty input; mixed
  signs, signed zero, subnormal scores, duplicate rows, and boundary ties.
  Add one-condition-away misses for each rejected structural dimension and
  exact-capacity limits (`2^53`, `2^53+1`).
- [ ] **VAL-03 — Runtime and parent-context semantics.** Depends: VAL-02,
  DES-09. Cover NULL, NaN, infinity, deterministic throwing expressions,
  unused `x`, capacity zero, parent projections, filters, joins, aggregation,
  and late invalid rows under outer `LIMIT 1` or `COUNT(*)`. Verify validation
  reads exactly the relevant DECIDE input rows and no outer filter changes the
  assignment; outer `LIMIT 0` need not execute it.
- [ ] **VAL-04 — Plan/serialization integration.** Depends: VAL-01, VAL-03.
  Verify logical bindings, physical output order/types, prepared statements,
  serializer round trips, and selected-rule explanation in default physical
  `EXPLAIN` and profiling. A selected plan must contain no solver selection,
  prepared model, or `PhysicalDecide`.
- [ ] **VAL-05 — Independent and backend differential.** Depends: VAL-02 through
  VAL-04. Compare direct with forced-solver runs on Gurobi and HiGHS where
  available, and with an independent oracle on representative cases. Compare
  exact feasibility and primary objective within a tolerance measured from
  backend behavior, not tied vectors. The independent oracle must check the
  finite-DOUBLE mathematical optimum, including tiny coefficients where
  backends can disagree. Run `make decide-test`, a forced-HiGHS pass, and
  DECIDE C++ regressions; disclose unavailable solver coverage.

**Exit gate:** all admitted cases preserve the observable contract, all
non-admitted cases use the unchanged solver path, and serializer/prepared-plan
coverage shows the boundary is not tied to one CLI execution shape.
