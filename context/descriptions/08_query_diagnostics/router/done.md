# Query Diagnostics — The Router (how it works)

The router is the post-solve dispatch spine: it classifies a solve outcome into the
one terminal the operator should route to. As of Batch 1 the **seam and the
unbounded terminal** are shipped (R1 / R2); the inf/unb check-ray disambiguation
(R3 / R4) and the infeasible / time_limit terminals (R5 / R6) are still in `todo.md`.

## The classifier — `RouteSolveResult`

`RouteSolveResult(const SolverResult &, const string &mode)` (`decide_router.hpp` /
`src/decidb/utility/decide_router.cpp`) is a **pure** function: status + the
`diagnose_decide` mode → a `DiagnosisTerminal` leaf. It owns no engine invocation
and no DuckDB execution/operator types, so the decision tree is unit-testable in
isolation (`test/common/test_decidb_router.cpp`, both modes × every status).

`enum class DiagnosisTerminal { SOLVED, UNBOUNDED, INFEASIBLE, TIME_LIMIT, UNDIAGNOSED }`:

| Solve status | `auto` mode | `off` mode |
| --- | --- | --- |
| `OPTIMAL` | `SOLVED` | `SOLVED` |
| `UNBOUNDED` | `UNBOUNDED` | `UNDIAGNOSED` |
| `INFEASIBLE` | `INFEASIBLE` | `UNDIAGNOSED` |
| `TIME_LIMIT` | `TIME_LIMIT` | `UNDIAGNOSED` |
| `INF_OR_UNBD` (residual) / `ITERATION_LIMIT` / `OTHER` | `UNDIAGNOSED` | `UNDIAGNOSED` |

The mode policy is not duplicated — the classifier defers to the existing
`DiagnosisApplies(mode, status)` gate (`decide_diagnostic.hpp`). `INFEASIBLE` and
`TIME_LIMIT` are classified as distinct leaves today even though no engine is wired
for them yet; this is what lets R5 / R6 drop their engines in without editing the
classifier.

## The operator switch (engine selection)

`PhysicalDecide::Finalize` (`physical_decide.cpp`) calls `RouteSolveResult` once and
`switch`es on the terminal — the **single** dispatch site:

- `SOLVED` → store the solution + indexer (and `ClearDecideDiagnostic`).
- `UNBOUNDED` → build the per-variable labels and the categorical-candidate provider,
  run `DiagnoseUnbounded`, stash + `ThrowDecideDiagnosisReady` on a populated
  diagnosis, else `ThrowUnboundedDiagnosisUnavailable` (see `unbounded/done.md`).
- `INFEASIBLE` / `TIME_LIMIT` / `UNDIAGNOSED` → the plain static solver error
  (`ThrowDecideSolveError`). `INFEASIBLE` / `TIME_LIMIT` get real engines at R5 / R6;
  they share the static-error arm until then.

This **replaced** the previous ad-hoc guard
(`DiagnosisApplies(mode, status) && status == UNBOUNDED`), which hand-rolled the
"engine exists" check inline — both jobs (mode gate + engine selection) now live in
the classifier and the switch.

### Candidate-assembly helper (operator-side)

The unbounded engine needs categorical groupings to characterize *which* instances
escape. Assembling them is operator-bound (it reads the executor chunks, entity
scopes, and `BuildGroupIds`), so it cannot live in the pure router. It was extracted
out of `Finalize` into a stateful functor `UnboundedCandidateProvider` +
`BuildUnboundedCandidateProvider(...)` in `physical_decide.cpp` (just above
`Finalize`), so `Finalize` only calls it. The functor caches its row-scoped and
per-scope entity candidates across the engine's per-variable calls.

## Still split out (Batch 2+ removes / absorbs these)

- **`INF_OR_UNBD` disambiguation** still lives in the facade — `DisambiguateInfOrUnbd`
  in `ilp_solver.cpp` resolves the ambiguous status via a zero-objective feasibility
  probe *before* the operator sees the result (`foundations/done.md`). R3 moves this
  into the router as the `check ray` branch and R4 removes the facade probe.
- **The infeasible / time_limit terminals** route to the static error until their
  engines land (R5 / R6).
