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

Covered by `test_query_diagnostics_f1.py:105-128`, which pins the HiGHS
MILP-unbounded case to the definitive unbounded message instead of the old
ambiguous text.

## Remaining Unbounded Work

Otherwise: unbounded currently throws the shared static paragraph in
`ThrowDecideSolveError` (`ilp_solver.cpp:88-96`). Ray extraction / diagnosis is
not implemented.
