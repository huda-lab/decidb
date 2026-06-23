# Query Diagnostics — The Router

The **router** is the single post-solve dispatch layer. After the solver returns a
`SolverResult`, the router inspects the status (plus a few sub-signals — is there a
recession ray? an incumbent?) and routes to exactly **one terminal**: an engine
followed by a report, or a direct report. It is the one place that decides "what
does this solve outcome mean, and what do we do about it."

It **replaces** the post-solve logic that was scattered across three places:

- the facade's inf/unb disambiguation re-solve (`DisambiguateInfOrUnbd` in
  `ilp_solver.cpp` — Gurobi `DualReductions=0`, HiGHS zero-objective probe) — *still
  in the facade; R3/R4 move it in*,
- the `DiagnosisApplies(mode, status)` filter gate — *now a shared helper the
  classifier defers to*, and
- the engine-selection / fall-through branch in `PhysicalDecide::Finalize`
  (`physical_decide.cpp`) — **done** (Batch 1): unified behind `RouteSolveResult` +
  the operator `switch` (`done.md`).

Those become one explicit decision tree. In particular, **the separate inf/unb
disambiguation probe is removed**: the router disambiguates "infeasible or
unbounded" by *extracting a ray* (`check ray`) instead of paying for a second
status-only re-solve — the ray we extract is itself the unboundedness witness, and
when present it also drives the report, so the work does double duty.

## The decision tree

The full dispatch tree is the area's single organizing diagram — it lives in the
[area README](../README.md) so there is one map of the whole system. This doc covers
the spine's own detail: each terminal's behavior, why the inf/unb branch uses a ray,
and the migration off today's scattered logic.

## Terminals

| Path | Terminal action |
|------|-----------------|
| solved | Normal success; no diagnosis. |
| failed → unbounded | `find ray`, then **report** the escaping variables + forced remedy (the shipped unbounded engine). |
| failed → infeasible | `elastic` (the infeasible engine), then **report** the relaxation. |
| failed → inf/unb → ray **found** | **report exactly as standard unbounded** (name the escaping variables + prescribe a bound), with one added caveat: *the problem may still be infeasible*. |
| failed → inf/unb → ray **not found** | `elastic`, then **report** (no improving direction exists, so it resolves toward infeasible). |
| time_limit → **incumbent** present | **report** the incumbent + optimality gap. |
| time_limit → **no sol** | **report slow** (no feasible solution found within the limit). |

## Why a ray is the right disambiguator for inf/unb

`check ray` reuses the portable box-LP ray extraction already built for the
unbounded engine (`BuildUnboundedRayFallbackModel`, `diagnostic_solves.cpp`), so it
is **solver-agnostic** by construction — no dependence on Gurobi-only status APIs.
A found ray is a sound witness that an improving recession direction exists; the
absence of one means the openness the presolve feared isn't there, so the failure
is infeasibility. This collapses the old "re-solve just to learn the status" step
into the same work that produces the user-facing diagnosis.

A found ray means unbounded *if a feasible point exists* — and the inf/unb path is
exactly the case where the solver never established feasibility. The router does
not pay for a separate feasibility check on the found branch; instead it reports as
standard unbounded and always appends *"the problem may still be infeasible."* The
caveat is therefore inherent to inf/unb (not specific to integer models, though a
MILP gives a second reason it can hold).

## Status & dependencies

The router is the integration point for every per-state engine, so it lands
incrementally as those engines exist:

- **the seam + unbounded terminal** — **shipped** (Batch 1, `done.md`): the
  `RouteSolveResult` classifier and the operator `switch`, with the `find ray` +
  report unbounded engine behind the `UNBOUNDED` terminal.
- **elastic / infeasible** engine — **not built** (`infeasible/`): until it lands,
  the `infeasible` and inf/unb-`not found` branches fall through to the static
  error rather than producing a relaxation report.
- **slow / time_limit** handling — **not built** (`slow/`): the `incumbent`/`no sol`
  split and its reports are future work.

The `diagnose_decide` setting still gates the whole router: `auto` (default) runs
it on a failed solve; `off` suppresses it and reproduces the plain static solver
error (see `foundations/done.md`).

## Docs in this folder

- `README.md` — this overview + per-terminal detail (the tree itself is in the area README).
- `todo.md` — the work-queue: per-terminal behavior, the migration off the current
  scattered logic, and open design questions.
- `done.md` — filled in as the router ships.
