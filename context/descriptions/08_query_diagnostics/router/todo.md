# Router — todo

The router is the unified post-solve dispatch (see `README.md` for the tree and
rationale). It **replaces** today's scattered logic: the facade's inf/unb
disambiguation re-solve (`DisambiguateInfOrUnbd`, `ilp_solver.cpp`), the
`DiagnosisApplies(mode, status)` gate, and the engine-selection / fall-through
branch in `PhysicalDecide::Finalize` (`physical_decide.cpp`).

Tasks are individually pickable. Each carries its pointers, the decision it
settles (if any), how to test it, and which `done.md` section to write when it
lands. Suggested batches at the bottom.

---

## R1 — Decide where the router lives, stand up the seam

- **What:** introduce a single `RouteSolveResult(...)` entry point that takes the
  `SolverResult` (+ the bits it needs: model/indexer for ray extraction, the
  `diagnose_decide` mode, context for stashing) and returns/throws the terminal
  action. No behavior change yet — it just wraps the current branching.
- **Decision (open):** does the router live in the operator (`physical_decide.cpp`,
  where the engines and stash already are) or as its own unit in
  `src/decidb/utility/` (testable without the execution stack, mirroring
  `diagnostic_solves.cpp`)? Lean: its own unit, so the decision tree is unit-testable
  with injected `SolverResult`s; the operator calls it.
- **Pointers:** `src/execution/operator/decide/physical_decide.cpp` (current
  dispatch ~5230–5390), `src/decidb/utility/decide_diagnostic.cpp` (gate helpers).
- **Test:** none yet (pure refactor) — existing diagnostics suite must stay green.
- **Done section:** "Where the router lives / the seam."

## R2 — Move the failed→unbounded path into the router

- **What:** the shipped unbounded engine (`find ray` → report) becomes the
  `UNBOUNDED` terminal of the router. Behavior identical; just relocated behind
  `RouteSolveResult`.
- **Pointers:** unbounded engine entry `DiagnoseUnbounded` (`physical_decide.cpp`),
  `unbounded/done.md`.
- **Test:** `test_query_diagnostics_f4/f5/f6/escaping_instances.py` stay green.
- **Done section:** "Terminals: unbounded."

## R3 — inf/unb → check ray (the new disambiguation)

- **What:** on `INF_OR_UNBD`, run the box-LP ray extraction
  (`BuildUnboundedRayFallbackModel`, `diagnostic_solves.cpp`).
  - **ray found** → report exactly as standard unbounded (reuse the R2 path:
    escaping variables + prescribe a bound) and **always append the caveat
    "the problem may still be infeasible."**
  - **ray not found** → infeasible terminal (R5; until elastic exists, the static
    infeasible error).
- **Decision (settled):** the caveat is **always** appended on the found branch —
  it is inherent to inf/unb (feasibility was never established), not MILP-specific.
  The router does **not** run a separate feasibility check on the found branch
  (one ray solve + honest caveat).
- **Pointers:** `diagnostic_solves.cpp` (ray fallback), the unbounded report
  formatter `BuildUnboundedDiagnostic` (`decide_diagnostic.cpp`).
- **Test:** new cases under both backends — an inf/unb model with a ray (assert
  unbounded-style report **+** the "may still be infeasible" caveat) and one
  without (assert it lands on the infeasible terminal). Confirm against
  `oracle_solver` / pinned constructed cases.
- **Done section:** "Terminals: inf/unb (check ray)."

## R4 — Remove the facade inf/unb disambiguation probe

- **What:** delete `DisambiguateInfOrUnbd` and its call in `SolveModel`
  (`ilp_solver.cpp`) — the router's `check ray` (R3) subsumes it. The facade now
  returns the raw `INF_OR_UNBD` status to the operator.
- **Decision (open / verify first):** confirm **both** backends still surface a
  terminal `INF_OR_UNBD` (rather than erroring or hanging) once the
  `DualReductions=0` (Gurobi) / zero-objective (HiGHS) probe is gone. If a backend
  cannot, keep a thin status-normalization shim but still route through `check ray`.
- **Pointers:** `ilp_solver.cpp` (`SolveModel`, `DisambiguateInfOrUnbd`),
  `gurobi_solver.cpp`, `deterministic_naive.cpp`, `solver_result.hpp` (the
  `INF_OR_UNBD` enum doc).
- **Test:** the R3 inf/unb cases now exercise the full path with the probe removed.
- **Done section:** "Replacing the facade probe."

## R5 — failed→infeasible → elastic terminal (blocked on the infeasible engine)

- **What:** wire the `INFEASIBLE` terminal to the elastic engine → report. Until
  the engine exists (`infeasible/`), this terminal throws the static infeasible
  error (current behavior), so the router structure is ready for it to drop in.
- **Blocked by:** the elastic/infeasible engine (`infeasible/todo.md`).
- **Test:** placeholder — assert infeasible still reaches the static error until the
  engine lands; full report test ships with the engine.
- **Done section:** "Terminals: infeasible (elastic)."

## R6 — time_limit terminal: incumbent vs no-sol (blocked on slow groundwork)

- **What:** split `TIME_LIMIT` on whether the solver returned a feasible incumbent:
  - **incumbent present** → report incumbent + optimality gap.
  - **no solution** → report slow.
- **Blocked by:** slow-state groundwork (`slow/`), including the 🔬 HiGHS
  time-limit / incumbent / gap behavior probes flagged in the area todos.
- **Pointers:** `SolverResult` incumbent/gap fields (`solver_result.hpp`).
- **Test:** ships with the slow work.
- **Done section:** "Terminals: time_limit."

## R7 — Router decision-tree unit tests

- **What:** unit-test `RouteSolveResult` against injected `SolverResult`s covering
  every leaf of the tree (solved, unbounded, infeasible, inf/unb±ray, time_limit
  incumbent / no-sol), independent of the execution stack. Depends on R1's seam
  being unit-testable.
- **Pointers:** mirror `test/common/test_decidb_diagnostic_solves.cpp` /
  `test_decidb_diagnostic_engines.cpp`.
- **Done section:** "Tests."

---

## Suggested batches

- **Batch 1 (refactor, no behavior change):** R1 + R2 + R7-partial — stand up the
  seam, move the unbounded path in, unit-test the leaves that exist. Ship behind
  green existing tests.
- **Batch 2 (the inf/unb win):** R3 + R4 — check-ray disambiguation in, facade
  probe out. This is the user-visible payoff and the reason the router exists.
- **Batch 3 (blocked, lands with their engines):** R5 with the infeasible engine,
  R6 with the slow groundwork.
