# Query Diagnostics — The Router

The **router** is the single post-solve dispatch layer. After the solver returns a
`SolverResult`, the router inspects the status (plus a few sub-signals — is there a
recession ray? an incumbent?) and routes to exactly **one terminal**: an engine
followed by a report, or a direct report. It is the one place that decides "what
does this solve outcome mean, and what do we do about it."

It centralizes the post-solve dispatch that used to be scattered across multiple
places:

- the facade's inf/unb disambiguation re-solve (`DisambiguateInfOrUnbd` in
  `ilp_solver.cpp` — zero-objective feasibility probe) — *retained: it keeps
  concrete statuses concrete before the router sees the result*,
- the `DiagnosisApplies(armed, status)` statement gate — *now a shared helper the
  classifier defers to*, and
- the engine-selection / fall-through branch in `PhysicalDecide::Finalize`
  (`physical_decide.cpp`) — **done** (Batch 1): unified behind `RouteSolveResult` +
  the operator `switch` (`done.md`).

The dispatch pieces now form one explicit decision tree. The router's inf/unb branch is a
**fallback**: existing solver/facade probes still try to return concrete
`UNBOUNDED` / `INFEASIBLE` first. If `INF_OR_UNBD` survives, the router extracts a
ray (`check ray`) and uses that sub-signal to choose the terminal.

## The decision tree

The full dispatch tree is the area's single organizing diagram — it lives in the
[area README](../README.md) so there is one map of the whole system. This doc covers
the spine's own detail: each terminal's behavior, why the inf/unb branch uses a ray,
and the shipped replacement for the former scattered logic.

## Terminals

| Path | Terminal action |
|------|-----------------|
| solved | Normal success; no diagnosis. |
| failed → unbounded | `find ray`, then **report** the escaping variables + forced remedy (the shipped unbounded engine). |
| failed → infeasible | `elastic` (the infeasible engine), then **report** the relaxation. |
| failed → inf/unb → ray **found** | **report exactly as standard unbounded** (name the escaping variables + prescribe a bound). When the engine can name no variable, the `undiagnosed` finding reports the state as `infeasible or unbounded` so the ambiguity is not hidden. |
| failed → inf/unb → ray **not found** | `elastic`, then **report** (no improving direction exists, so it resolves toward infeasible). |

A time limit is not on this list. It is handled before the router runs — see
`../../01_pipeline/08_execution/slow_solves.md`.

## Why a ray is the right disambiguator for inf/unb

`check ray` reuses the portable box-LP ray extraction already built for the
unbounded engine (`BuildUnboundedRayFallbackModel`, `probe_models.cpp`), so it
is **solver-agnostic** by construction — no dependence on Gurobi-only status APIs.
A found ray is a sound witness that an improving recession direction exists; the
absence of one means the openness the presolve feared isn't there, so the failure
resolves toward infeasibility.

A found ray means unbounded *if a feasible point exists* — and the inf/unb path is
exactly the case where the solver never established feasibility. The router does
not pay for a separate feasibility check on the found branch. When the engine names
an escaping variable, it returns the standard unbounded findings. When it cannot,
the fallback finding keeps `state='infeasible or unbounded'` so the unresolved status
is not hidden.

## Status & dependencies

The router is the integration point for every per-state engine, so it lands
incrementally as those engines exist:

- **the seam + unbounded terminal** — **shipped** (Batch 1, `done.md`): the
  `RouteSolveResult` classifier and the operator `switch`, with the `find ray` +
  report unbounded engine behind the `UNBOUNDED` terminal.
- **residual inf/unb check-ray routing** — **shipped** (Batch 2, `done.md`): if
  `INF_OR_UNBD` survives existing concrete-status probes, ray present routes to
  the unbounded terminal; no ray routes to infeasible. An unnamed ray retains the
  ambiguous state in its `undiagnosed` finding.
- **elastic / infeasible** engine — **shipped** (`infeasible/`): the `infeasible`
  and inf/unb-`not found` branches run the elastic engine and report the
  least-change relaxation when one is available.
- **slow / time_limit** — **no longer a terminal** (batch H). A time limit, and Ctrl-C,
  are ordinary execution behaviour: the operator handles them before the router runs, with
  or without the prefix. See `../../01_pipeline/08_execution/slow_solves.md`.

The `DIAGNOSE` statement prefix gates the whole router: with it, a failed solve routes to
its engine; without it, every failure collapses to `UNDIAGNOSED` and the plain static
solver error (see `foundations/done.md`).

## Docs in this folder

- `README.md` — this overview + per-terminal detail (the tree itself is in the area README).
- `todo.md` — records that no router work remains.
- `done.md` — filled in as the router ships.
