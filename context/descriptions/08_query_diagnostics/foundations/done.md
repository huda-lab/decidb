# Query Diagnostics — Foundations (implemented)

## F1 · Structured solver result — DONE

The solve path now returns a structured result carrying the terminal status, so
callers can branch on the outcome instead of catching an exception. This is the
gate for the rest of the area.

- **`SolverResult` + `SolverStatus`** — new header
  `src/include/duckdb/decidb/solver_result.hpp`. `SolverStatus` =
  `{OPTIMAL, INFEASIBLE, UNBOUNDED, INF_OR_UNBD, TIME_LIMIT, ITERATION_LIMIT, OTHER}`
  (`:23`); `SolverResult { status, solution, ray, raw_status }` (`:37`). `ray` is
  reserved for U2; `raw_status` carries the backend-native code for the OTHER
  message.
- **Backends map-and-return instead of throwing on solver status.**
  `GurobiSolver::Solve` and `DeterministicNaive::Solve` now return `SolverResult`
  (signatures in their headers). The non-optimal status blocks became switch maps
  (`gurobi_solver.cpp` ~`:222-251`, `:274`; `deterministic_naive.cpp` ~`:207-237`,
  `:262`). **HiGHS `kUnboundedOrInfeasible` (status 9) → `INF_OR_UNBD`**
  (`deterministic_naive.cpp:220-224`) — previously dropped into a generic "solver
  status %d" catch-all. Genuine internal/API errors (NaN/Inf, extraction, API
  failures) still throw.
- **`SolveModel` facade returns `SolverResult`** and no longer throws on solver
  status (`ilp_solver.cpp:12`). The default user-facing error text — formerly
  duplicated verbatim across both backends — is now a single helper
  **`ThrowDecideSolveError(const SolverResult &)`** (`ilp_solver.cpp:39`, declared
  in `solver_result.hpp:53`).
- **Throw relocated to the operator (F4-ready).** `PhysicalDecide::Finalize`
  branches on status: non-optimal → `ThrowDecideSolveError` (manual-first); optimal
  → `gstate.ilp_solution = std::move(solve_result.solution)`
  (`physical_decide.cpp:5027-5034`). The F4 pragma will later gate that one call
  site to route the status into diagnosis instead of throwing.
- **Behavior unchanged for users:** same errors, same wording (the `INF_OR_UNBD`
  message already says "infeasible or unbounded"). Covered by dedicated F1
  regressions in `test/decide/tests/test_query_diagnostics_f1.py`: optimal
  solution extraction on both backends; HiGHS infeasible / unbounded /
  `INF_OR_UNBD` paths; and Gurobi infeasible / unbounded paths. The older
  HiGHS-pinned regression remains in
  `test/decide/tests/test_error_infeasible.py::test_highs_milp_unbounded_reports_unbounded`.
- **Deferred (not F1):** timeout incumbent / objective / best-bound / gap fields
  (slow branch, S2); filling `ray` (U2); the pragma gate (F4); HiGHS `time_limit`
  honoring (slow branch).

## Baseline for the remaining foundations (F2–F6)

- **`ModelConstraint` has no provenance** — only indices / coefficients / sense /
  rhs (`src/include/duckdb/decidb/ilp_model.hpp:78-83`). F2 adds it.
- **Variable names die at the solver boundary** — `VarIndexer` maps
  `(decide_var_idx, row)` → flat index but stores no names; aliases live only in
  `LogicalDecide.decide_variables[*].alias`. F6 threads them through.
- **HiGHS sets no time limit** (Gurobi reads `DECIDB_TIME_LIMIT`, 300s default,
  `gurobi_solver.cpp:70-81`). Honoring it on HiGHS is a slow-branch item, **not**
  F1.
