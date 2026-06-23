# Query Diagnostics — The Router (how it works)

The router is the post-solve dispatch spine: it classifies a solve outcome into the
one terminal the operator should route to. Batch 1 shipped the **seam and the
unbounded terminal** (R1 / R2); Batch 2 shipped residual `INF_OR_UNBD` check-ray
routing (R3). The infeasible / time_limit terminals (R5 / R6) are still in
`todo.md`.

## The classifier — `RouteSolveResult`

`RouteSolveResult(const SolverResult &, const string &mode)` (`decide_router.hpp` /
`src/decidb/utility/decide_router.cpp`) is a **pure** function: status + the
`diagnose_decide` mode + the residual `INF_OR_UNBD` ray sub-signal →
`DiagnosisTerminal`. It owns no engine invocation and no DuckDB execution/operator
types, so the decision tree is unit-testable in isolation
(`test/common/test_decidb_router.cpp`).

`enum class DiagnosisTerminal { SOLVED, UNBOUNDED, INFEASIBLE, TIME_LIMIT, UNDIAGNOSED }`:

| Solve status | `auto` mode | `off` mode |
| --- | --- | --- |
| `OPTIMAL` | `SOLVED` | `SOLVED` |
| `UNBOUNDED` | `UNBOUNDED` | `UNDIAGNOSED` |
| `INFEASIBLE` | `INFEASIBLE` | `UNDIAGNOSED` |
| `TIME_LIMIT` | `TIME_LIMIT` | `UNDIAGNOSED` |
| `INF_OR_UNBD` (residual, ray present) | `UNBOUNDED` | `UNDIAGNOSED` |
| `INF_OR_UNBD` (residual, no ray) | `INFEASIBLE` | `UNDIAGNOSED` |
| `ITERATION_LIMIT` / `OTHER` | `UNDIAGNOSED` | `UNDIAGNOSED` |

The mode policy is not duplicated — the classifier defers to the existing
`DiagnosisApplies(mode, status)` gate (`decide_diagnostic.hpp`). `INFEASIBLE` and
`TIME_LIMIT` are classified as distinct leaves today even though no engine is wired
for them yet; this is what lets R5 / R6 drop their engines in without editing the
classifier.

## Terminals: inf/unb (check ray)

Existing concrete-status probes stay in place: Gurobi may run its native
`DualReductions=0` retry, and the facade's `DisambiguateInfOrUnbd` still runs the
zero-objective feasibility probe. The router only sees `INF_OR_UNBD` when that
status survives those attempts.

Under `diagnose_decide='auto'`, `SolveModel` asks the same portable box-LP ray
fallback used by the unbounded engine to attach a ray for `UNBOUNDED` **or**
residual `INF_OR_UNBD`. The router then treats residual `INF_OR_UNBD` as:

- **ray present** → route to the existing unbounded terminal. The stashed
  `decide_diagnostics()` rows stay exactly the standard unbounded rows; the
  thrown query error appends the caveat `the problem may still be infeasible.`
- **no ray** → route to the infeasible terminal. Until the elastic engine lands,
  this means the current static infeasible error.

`diagnose_decide='off'` still suppresses both branches and returns the plain
static `INF_OR_UNBD` error.

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

## Still split out

- **The infeasible / time_limit terminals** route to the static error until their
  engines land (R5 / R6).
