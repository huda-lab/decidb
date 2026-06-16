# Query Diagnostics — Unbounded (implemented)

## Gurobi INF_OR_UNBD disambiguation — DONE

When Gurobi presolve returns the
ambiguous `GRB_INF_OR_UNBD`, DeciDB re-solves with `DualReductions=0`, yielding a
definitive `INFEASIBLE` or `UNBOUNDED` (the extra solve only happens in this rare
ambiguous case). `src/decidb/gurobi/gurobi_solver.cpp:202-219`; residual throw if
the disambiguation itself fails at `246-253`. Landed in commit `9d4bbd59f0`.

So U2/U3 receive an already-disambiguated status on Gurobi.

## U1 · HiGHS INF_OR_UNBD disambiguation — DONE

HiGHS has no native MIP disambiguation equivalent to Gurobi's
`DualReductions=0`. The linked HiGHS source allows `kUnboundedOrInfeasible` for
MIP results, and its stricter `allow_unbounded_or_infeasible=false` cleanup path
only forces LP primal-simplex disambiguation. Native `getPrimalRay` is also not a
classifier because it clears integrality and solves an LP relaxation.

U1 provides the portable classifier: when any backend returns normalized
`SolverStatus::INF_OR_UNBD`, DeciDB runs a prepared zero-objective probe on the
same `SolverModel` and same selected backend.

- `OPTIMAL` ⇒ original model is feasible, so report `UNBOUNDED`.
- `INFEASIBLE` ⇒ report `INFEASIBLE`.
- Any other probe result preserves the original ambiguous status.

The probe model preserves variables, bounds, integrality, and constraints while
clearing linear and quadratic objective terms (`diagnostic_solves.cpp:5-16`).
The solve facade builds the model once, selects the backend once, exposes
prepared-model solving for future diagnostic reruns, and performs the U1
classification before the operator sees the result (`ilp_solver.hpp:20-33`,
`ilp_solver.cpp:13-75`).

Covered by `test_query_diagnostics_f1.py:117-142`, which pins the HiGHS
MILP-unbounded case to the definitive unbounded message instead of the old
ambiguous text — for both MAXIMIZE and MINIMIZE, since the zero-objective probe
is sense-agnostic.

## U2 · Portable fallback ray extraction — DONE

U2 is implemented as a diagnostic-only follow-up solve over the prepared
`SolverModel`; it is not wired to user-facing diagnosis yet.

- `SolveModelOptions::extract_unbounded_ray` in
  `src/include/duckdb/decidb/ilp_solver.hpp` is the internal opt-in. The existing
  two-argument `SolveModel(input, indexer)` overload keeps default failed-query
  behavior unchanged and does not pay for ray extraction.
- `BuildUnboundedRayFallbackModel` in
  `src/decidb/utility/diagnostic_solves.cpp` builds the bounded LP:
  `maximize signed(c)^T d`, preserves each linear row sense with RHS `0`, relaxes
  all variables to continuous, and sets `0 <= d_i <= 1` only when the original
  upper bound is effectively infinite (`>= 1e20`). Finite-upper-bound columns are
  fixed to `d_i = 0`.
- Quadratic objectives and quadratic constraints are intentionally out of scope
  for U2 v1. The helper returns false and leaves `SolverResult::ray` empty.
- `src/decidb/utility/ilp_solver.cpp` invokes the fallback only after U1 has
  resolved the primary result to `SolverStatus::UNBOUNDED`, solves the ray LP on
  the same backend, and attaches `SolverResult::ray` only when the ray LP is
  `OPTIMAL` and signed objective improvement is greater than `1e-8`.
- Native Gurobi/HiGHS ray APIs remain deferred as optional accelerators; U2 owns
  the portable box-LP path first.

Covered by `test/common/test_decidb_diagnostic_solves.cpp`, test case
`DeciDB query diagnostics unbounded ray fallback`: symmetric MAX full-support
ray, signed MIN objective, finite-upper-bound zeroing, bounded-shape no-ray,
`>=` / `=` row-sense preservation in homogenization, integrality relaxation, and
opt-in-only `SolveModel` attachment.

## Remaining Unbounded Work

Otherwise: unbounded currently throws the shared static paragraph in
`ThrowDecideSolveError` (`src/decidb/utility/ilp_solver.cpp`). Ray extraction is
available internally when requested, but ray-to-SQL mapping, the diagnostic
pragma, and user-facing reporting are still U3/F4/F5/F6 work.
