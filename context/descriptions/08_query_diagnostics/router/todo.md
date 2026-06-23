# Router — todo

The router is the unified post-solve dispatch (see `README.md` for the tree and
rationale). **Batch 1 shipped** the seam (`RouteSolveResult`) and the unbounded
terminal (R1 / R2) — see `done.md`. It still has to **replace** the remaining
scattered logic: the facade's inf/unb disambiguation re-solve
(`DisambiguateInfOrUnbd`, `ilp_solver.cpp`) and wire the infeasible / time_limit
terminals to real engines.

Tasks are individually pickable. Each carries its pointers, the decision it
settles (if any), how to test it, and which `done.md` section to write when it
lands. Suggested batches at the bottom.

---

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

## R7 — Router decision-tree unit tests (remaining leaves)

- **Shipped (Batch 1):** the leaves that exist today — `SOLVED`, `UNBOUNDED`,
  `INFEASIBLE`, `TIME_LIMIT`, `UNDIAGNOSED` (residual `INF_OR_UNBD` /
  `ITERATION_LIMIT` / `OTHER`) — are unit-tested across both modes in
  `test/common/test_decidb_router.cpp`.
- **What remains:** extend coverage as new leaves/sub-signals land —
  inf/unb ± ray (R3) and time_limit incumbent / no-sol (R6) — once those branches
  read sub-signals beyond the bare status.
- **Pointers:** `test/common/test_decidb_router.cpp` (mirror its pattern).
- **Done section:** fold into "Tests."

---

## Suggested batches

- ~~**Batch 1 (refactor, no behavior change):** R1 + R2 + R7-partial~~ — **shipped**
  (`done.md`): the seam, the unbounded terminal, and the unit-tested existing leaves.
- **Batch 2 (the inf/unb win):** R3 + R4 — check-ray disambiguation in, facade
  probe out. This is the user-visible payoff and the reason the router exists.
- **Batch 3 (blocked, lands with their engines):** R5 with the infeasible engine,
  R6 with the slow groundwork.
