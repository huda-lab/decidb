# Router

The router is the one post-solve dispatch layer. After the solver returns a `SolverResult`, the router reads the
status and one sub-signal (is there a recession ray?) and routes to exactly one terminal: an engine followed by a
report, or a direct report. The dispatch tree is drawn in [`../README.md`](../README.md).

The `DIAGNOSE` prefix gates the whole router: with it, a failed solve routes to its engine; without it, every failure
collapses to `UNDIAGNOSED` and the plain solver error (`../foundations/done.md`). A time limit or Ctrl-C is not a
terminal: it happens with or without the prefix, no engine runs, and the operator handles it before the router runs
(`../../pipeline/08_execution/slow_solves.md`).

## The classifier — `RouteSolveResult`

`RouteSolveResult(const SolverResult &, bool armed)` (`decide_router.hpp` /
`src/decidb/diagnostics/decide_router.cpp`) is a **pure** function: status + the DIAGNOSE prefix + the residual
`INF_OR_UNBD` ray sub-signal → `DiagnosisTerminal`. It owns no engine invocation and no DuckDB execution or operator
types, so the decision tree is unit-testable in isolation, and an engine can be added or removed behind a leaf without
editing the classifier.

`enum class DiagnosisTerminal { SOLVED, UNBOUNDED, INFEASIBLE, UNDIAGNOSED }`:

| Solve status | under `DIAGNOSE` | unprefixed |
| --- | --- | --- |
| `OPTIMAL`, `SUBOPTIMAL` | `SOLVED` | `SOLVED` |
| `UNBOUNDED` | `UNBOUNDED` | `UNDIAGNOSED` |
| `INFEASIBLE` | `INFEASIBLE` | `UNDIAGNOSED` |
| `INF_OR_UNBD` (residual, ray present) | `UNBOUNDED` | `UNDIAGNOSED` |
| `INF_OR_UNBD` (residual, no ray) | `INFEASIBLE` | `UNDIAGNOSED` |
| `TIME_LIMIT` | `UNDIAGNOSED` | `UNDIAGNOSED` |
| `ITERATION_LIMIT` / `OTHER` | `UNDIAGNOSED` | `UNDIAGNOSED` |

A `SUBOPTIMAL` result carries a feasible incumbent, so it is delivered as a success. `TIME_LIMIT` is listed only so
`-Wswitch` keeps flagging it: the operator handles a timeout before calling the router, so reaching that row means the
timeout was not handled and the plain error is the right answer.

## The inf/unb branch

The facade's `DisambiguateInfOrUnbd` (`ilp_solver.cpp`) runs a zero-objective feasibility probe on every `INF_OR_UNBD`
(`../../pipeline/07_solver/done.md`). The router sees `INF_OR_UNBD` only when that status survives the probe. Under
`DIAGNOSE`, `SolveModel` attaches a ray from the unbounded engine's portable box-LP fallback
(`BuildUnboundedRayFallbackModel`, `probe_models.cpp`) for `UNBOUNDED` **or** residual `INF_OR_UNBD`. The router then
treats residual `INF_OR_UNBD` as:

- **ray present** → the unbounded terminal, with the standard unbounded findings. When the engine cannot name a
  variable, the `undiagnosed` finding keeps `state = 'infeasible or unbounded'` so the ambiguity is not hidden.
- **no ray** → the infeasible terminal: no improving direction exists, so the failure resolves toward infeasible.

An unprefixed query suppresses both branches and returns the plain `INF_OR_UNBD` error.

**Why a ray decides.** The ray check uses no Gurobi-only status API. A found ray is a sound witness that an improving
direction exists; no ray means the openness presolve feared is not there. A found ray means unbounded only if a
feasible point exists, and the router does not pay for a separate feasibility check: when the engine cannot name a
variable, the finding keeps the ambiguous state.

## The operator switch (engine selection)

`PhysicalDecide::Finalize` (`physical_decide.cpp`) handles a `TIME_LIMIT` stop first, then calls `RouteSolveResult`
once and `switch`es on the terminal — the **single** dispatch site:

- `SOLVED` → store the solution and indexer (and `ClearDecideDiagnostic`, so a `DIAGNOSE` above this operator reports
  `feasible`). A `SUBOPTIMAL` result adds the "not proven best" caveat on stderr.
- `UNBOUNDED` → build the per-variable labels and the categorical-candidate provider, run `DiagnoseUnbounded`, and
  `report(...)` the findings; when the engine names no variable, report an `undiagnosed` finding carrying the reason
  (`../unbounded/done.md`).
- `INFEASIBLE` → normalize a residual `INF_OR_UNBD` to `INFEASIBLE`, formulate the repair model
  (`FormulateElasticModel`), build the `InfeasibleDiagnosisInput` (that model, its indexer, labels and an injected
  solve callback), run `DiagnoseInfeasible`, and `report(...)` the findings; when the engine declines, report an
  `undiagnosed` finding saying whether diagnosis ran out of time or found no loosening that helps. The engine is
  documented in `../infeasible/done.md`.
- `UNDIAGNOSED` → the plain static solver error (`ThrowDecideSolveError`).

`report(...)` is the operator-local lambda that writes a diagnosis to the statement-scoped handoff and sets
`gstate.diagnosis_only`, so the operator emits no rows and the `DIAGNOSE` operator above consumes the findings as the
statement's answer. It is only reached under the prefix; an unprefixed failure always lands on `UNDIAGNOSED` and
raises. No call site checks inline whether an engine exists.

**Candidate provider.** The unbounded engine needs categorical groupings to characterize *which* instances escape.
Assembling them reads the executor chunks, the keyed decisions' key scopes (`KeyedScopeInfo`) and `BuildGroupIds`, so
it cannot live in the pure router. It is a stateful functor, `UnboundedCandidateProvider` (`physical_decide.cpp`),
constructed in the `UNBOUNDED` arm as the engine's `get_candidates`. It caches its per-row candidates and each key
scope's class candidates across the engine's per-variable calls.

## Tests

`test/common/test_decidb_router.cpp` tests the classifier with no solver or operator: every leaf with the prefix and
without it (all failures collapse to `UNDIAGNOSED`); residual `INF_OR_UNBD` by ray sub-signal; a time limit is never a
diagnosis terminal, prefix or no prefix.
