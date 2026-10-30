# Known bugs

## BUG-38 — Per-row quadratic and `<>` rows of a grouped clause name no group

- **Observed (lane AI, PERF-12, by reading the code):** the per-row linear rows of a grouped clause carry the group's
  name, so DIAGNOSE fills its `group` column; the per-row quadratic rows (`ilp_model_builder.cpp`, the quadratic per-row
  site) and the per-row `<>` rows (`linearization_not_equal.cpp` ~205) carry the group id but no name, so a finding on
  them would print an empty `group`.
- **Fix (to rule):** confirm with a DIAGNOSE query on each shape; then every emission site stamps the name through
  `StampConstraintProvenance`, as the linear one does.

## BUG-39 — A collated key's group spelling depends on clause order

- **Observed:** over rows `'a','A','B','b','b'` of `g COLLATE NOCASE`, `DIAGNOSE ... when id >= 4 per g: sum(x) by (g)
  >= 3 AND per g: sum(x) by (g) <= 1` in the expanded scope names one group `b` in the first finding and `B` in the
  second: each clause's representative row is the first row it reads.
- **Fix (to rule):** one spelling per group of a key, whatever rows a clause reads.

## Limitations (not bugs)

- **A repair exactly at the tolerance on HiGHS's QP path.** An edit whose rows miss a summed bound by exactly the
  feasibility tolerance (1e-6) passes the certificate's LP on both backends, and Gurobi solves the edited query; HiGHS's
  QP path rejects it. HiGHS judges the same violation differently in its LP and QP paths.
- **Gurobi's barrier QP at stage 2.** On a 9,000-row QP repair Gurobi returns a point that breaks a row by 5.7e-5 and an
  objective value 24 above the optimum HiGHS finds (225000010024 against 225000010000). Inside Gurobi.
- **HiGHS on the edited 9,000-row QP.** It reaches its 300 s limit where Gurobi solves it.

Resolved behavior is documented by its owning `done.md`.
