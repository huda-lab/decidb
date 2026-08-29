# Query Diagnostics — The Router (how it works)

The router is the post-solve dispatch spine: it classifies a solve outcome into the
one terminal the operator should route to. Batch 1 shipped the **seam and the
unbounded terminal** (R1 / R2); Batch 2 shipped residual `INF_OR_UNBD` check-ray
routing (R3); R5 wired the **infeasible terminal** to the elastic engine.

**Batch H changed what starts it and dropped a leaf.** The gate is no longer a session
mode but the statement's `DIAGNOSE` prefix, so the classifier takes a `bool`. And a time
limit stopped being a terminal: a slow solve is ordinary execution behaviour, handled by
the operator *before* the router runs (see
`../../01_pipeline/08_execution/slow_solves.md`). Four terminals became three.

## The classifier — `RouteSolveResult`

`RouteSolveResult(const SolverResult &, bool armed)` (`decide_router.hpp` /
`src/decidb/diagnostics/decide_router.cpp`) is a **pure** function: status + the DIAGNOSE
prefix + the residual `INF_OR_UNBD` ray sub-signal → `DiagnosisTerminal`. It owns no
engine invocation and no DuckDB execution/operator types, so the decision tree is
unit-testable in isolation (`test/common/test_decidb_router.cpp`).

`enum class DiagnosisTerminal { SOLVED, UNBOUNDED, INFEASIBLE, UNDIAGNOSED }`:

| Solve status | under `DIAGNOSE` | unprefixed |
| --- | --- | --- |
| `OPTIMAL` | `SOLVED` | `SOLVED` |
| `UNBOUNDED` | `UNBOUNDED` | `UNDIAGNOSED` |
| `INFEASIBLE` | `INFEASIBLE` | `UNDIAGNOSED` |
| `INF_OR_UNBD` (residual, ray present) | `UNBOUNDED` | `UNDIAGNOSED` |
| `INF_OR_UNBD` (residual, no ray) | `INFEASIBLE` | `UNDIAGNOSED` |
| `TIME_LIMIT` | `UNDIAGNOSED` | `UNDIAGNOSED` |
| `ITERATION_LIMIT` / `OTHER` | `UNDIAGNOSED` | `UNDIAGNOSED` |

`TIME_LIMIT` is listed only so `-Wswitch` keeps flagging it: the operator handles a
timeout before calling the router, so reaching that row means the timeout was not
handled and the plain error is the right answer.

The prefix policy is not duplicated — the classifier defers to
`DiagnosisApplies(armed, status)` (`decide_diagnostic.hpp`). Because the classifier owns
no engine invocation, R5 could drop the infeasible engine in behind an existing leaf
without editing the classifier, and batch H could remove a leaf the same way.

## Terminals: inf/unb (check ray)

Existing concrete-status probes stay in place: Gurobi may run its native
`DualReductions=0` retry, and the facade's `DisambiguateInfOrUnbd` still runs the
zero-objective feasibility probe. The router only sees `INF_OR_UNBD` when that
status survives those attempts.

Under `DIAGNOSE`, `SolveModel` asks the same portable box-LP ray
fallback used by the unbounded engine to attach a ray for `UNBOUNDED` **or**
residual `INF_OR_UNBD`. The router then treats residual `INF_OR_UNBD` as:

- **ray present** → route to the existing unbounded terminal. The findings are exactly
  the standard unbounded ones; when the engine cannot name a variable, the `undiagnosed`
  finding reports the state as `infeasible or unbounded` so the ambiguity is not hidden.
- **no ray** → route to the infeasible terminal (the elastic engine; see below).

An unprefixed query suppresses both branches and returns the plain static
`INF_OR_UNBD` error.

## The operator switch (engine selection)

`PhysicalDecide::Finalize` (`physical_decide.cpp`) handles a `TIME_LIMIT` stop first
(execution behaviour, not a diagnosis), then calls `RouteSolveResult` once and `switch`es
on the terminal — the **single** dispatch site:

- `SOLVED` → store the solution + indexer (and `ClearDecideDiagnostic`, so a `DIAGNOSE`
  above this operator reports `feasible`).
- `UNBOUNDED` → build the per-variable labels and the categorical-candidate provider,
  run `DiagnoseUnbounded`, `report(...)` the findings; when the engine names no variable,
  report an `undiagnosed` finding carrying the reason (see `unbounded/done.md`).
- `INFEASIBLE` → build the elastic-engine input and run `DiagnoseInfeasible`,
  `report(...)` the findings; when the engine declines, report an `undiagnosed` finding
  saying whether diagnosis ran out of time or found no loosening that helps.
- `UNDIAGNOSED` → the plain static solver error (`ThrowDecideSolveError`).

`report(...)` is the operator-local lambda that writes a diagnosis to the statement-scoped
handoff and sets `gstate.diagnosis_only`, so the operator emits no rows and the `DIAGNOSE`
operator above consumes the findings as the statement's answer. It is only ever reached
under the prefix; an unprefixed failure always lands on `UNDIAGNOSED` and raises.

This **replaced** the previous ad-hoc guard
(`DiagnosisApplies(mode, status) && status == UNBOUNDED`), which hand-rolled the
"engine exists" check inline — both jobs (the gate + engine selection) now live in
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
solve callback), and calls `DiagnoseInfeasible`. A valid diagnosis is reported as
findings; a helper-solve timeout or a declined diagnosis reports an `undiagnosed` finding
saying which it was. A residual `INF_OR_UNBD` (empty ray) is normalized to `INFEASIBLE`
here first. The engine itself — the elastic program,
per-shape slack placement, exact atomic DROP, and stage-2 achievable objective — is
documented in `infeasible/done.md`.

## Not a terminal: the time limit

`TIME_LIMIT` was a fourth terminal (R6, the slow engine). Batch H removed it. A solve
that runs out of wall-clock — or that a user stops with Ctrl-C — is ordinary execution
behaviour: it happens with or without the prefix, no engine runs, and the operator deals
with it before the router is called. Its report, its continuation offer, and the
Ctrl-C mechanics live in `../../01_pipeline/08_execution/slow_solves.md`.

## Tests

Because `RouteSolveResult` is a pure function, the decision tree is unit-tested in
isolation in `test/common/test_decidb_router.cpp` — no solver or operator needed:

- **Every classifier leaf, prefixed and not.** `SOLVED`, `UNBOUNDED`, `INFEASIBLE`, and
  `UNDIAGNOSED` (`ITERATION_LIMIT` / `OTHER`) are checked with the prefix (each maps to
  its terminal) and without (all failures collapse to `UNDIAGNOSED`, the plain static
  error).
- **Residual `INF_OR_UNBD` by ray sub-signal.** Under the prefix, ray-present routes to
  `UNBOUNDED` and no-ray routes to `INFEASIBLE`; unprefixed, both collapse.
- **A time limit is never a diagnosis terminal**, prefix or no prefix.

The classifier leaves stayed stable across the terminal-wiring batches: R5
(infeasible→elastic) dropped its engine in *behind* an existing leaf without editing the
classifier, and batch H removed the time-limit leaf the same way — the behaviour it
carried moved out whole, to `../../01_pipeline/08_execution/slow_solves.md`.
