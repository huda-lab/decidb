# Query Diagnostics — The Router (how it works)

The router is the post-solve dispatch spine: it classifies a solve outcome into the
one terminal the operator should route to. Batch 1 shipped the **seam and the
unbounded terminal** (R1 / R2); Batch 2 shipped residual `INF_OR_UNBD` check-ray
routing (R3); R5 wired the **infeasible terminal** to the elastic engine. The
time_limit terminal (R6) is still in `todo.md`.

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
`DiagnosisApplies(mode, status)` gate (`decide_diagnostic.hpp`). Because the
classifier owns no engine invocation, `TIME_LIMIT` is already a distinct leaf even
though the slow engine is not wired yet; this is what let R5 drop the infeasible
engine in (and lets R6 drop the slow engine in) without editing the classifier.

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
- **no ray** → route to the infeasible terminal (the elastic engine; see below).

`diagnose_decide='off'` still suppresses both branches and returns the plain
static `INF_OR_UNBD` error.

## The operator switch (engine selection)

`PhysicalDecide::Finalize` (`physical_decide.cpp`) calls `RouteSolveResult` once and
`switch`es on the terminal — the **single** dispatch site:

- `SOLVED` → store the solution + indexer (and `ClearDecideDiagnostic`).
- `UNBOUNDED` → build the per-variable labels and the categorical-candidate provider,
  run `DiagnoseUnbounded`, stash + `ThrowDecideDiagnosisReady` on a populated
  diagnosis, else `ThrowUnboundedDiagnosisUnavailable` (see `unbounded/done.md`).
- `INFEASIBLE` → build the elastic-engine input and run `DiagnoseInfeasible`, stash +
  `ThrowDecideDiagnosisReady` on a valid diagnosis, else `ThrowDecideSolveError` (see
  "Terminals: infeasible (elastic)" below).
- `TIME_LIMIT` / `UNDIAGNOSED` → the plain static solver error
  (`ThrowDecideSolveError`). `TIME_LIMIT` gets its real engine at R6; it shares the
  static-error arm until then.

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

## Terminals: infeasible (elastic)

R5 wired the `INFEASIBLE` terminal to the **elastic engine**. In the
`DiagnosisTerminal::INFEASIBLE` arm, `Finalize` re-emits absorbed user bounds as
slackable rows, resets implied bound tightenings to the intrinsic domain, builds the
`InfeasibleDiagnosisInput` (the retained `SolverModel` + indexer + labels + an injected
solve callback), and calls `DiagnoseInfeasible`. A valid diagnosis is stashed and
surfaced (`ThrowDecideDiagnosisReady`); otherwise the arm falls through to
`ThrowDecideSolveError`. A residual `INF_OR_UNBD` (empty ray) is normalized to
`INFEASIBLE` here before the message is built. The engine itself — the elastic program,
per-shape slack placement, removal dial, and stage-2 achievable objective — is
documented in `infeasible/done.md`.

## Still split out

- **The time_limit terminal** routes to the static error until its engine lands (R6).
