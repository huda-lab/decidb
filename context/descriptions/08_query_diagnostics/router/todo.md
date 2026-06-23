# Router — todo

The router is the unified post-solve dispatch (see `README.md` for the tree and
rationale). **Batch 1 shipped** the seam (`RouteSolveResult`) and the unbounded
terminal (R1 / R2); **Batch 2 shipped** residual `INF_OR_UNBD` check-ray routing
(R3) — see `done.md`. It still has to wire the infeasible / time_limit terminals
to real engines.

Tasks are individually pickable. Each carries its pointers, the decision it
settles (if any), how to test it, and which `done.md` section to write when it
lands. Suggested batches at the bottom.

---

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
  `INFEASIBLE`, `TIME_LIMIT`, `UNDIAGNOSED` (`ITERATION_LIMIT` / `OTHER`) — are
  unit-tested across both modes in
  `test/common/test_decidb_router.cpp`.
- **Shipped (Batch 2):** residual `INF_OR_UNBD` is unit-tested across `auto`/`off`
  and ray/no-ray sub-signals.
- **What remains:** extend coverage when time_limit incumbent / no-sol (R6) lands.
- **Pointers:** `test/common/test_decidb_router.cpp` (mirror its pattern).
- **Done section:** fold into "Tests."

---

## Suggested batches

- ~~**Batch 1 (refactor, no behavior change):** R1 + R2 + R7-partial~~ — **shipped**
  (`done.md`): the seam, the unbounded terminal, and the unit-tested existing leaves.
- ~~**Batch 2 (the residual inf/unb fallback):** R3~~ — **shipped** (`done.md`):
  existing concrete-status probes stay in place; only residual `INF_OR_UNBD`
  routes through check-ray.
- **Batch 3 (blocked, lands with their engines):** R5 with the infeasible engine,
  R6 with the slow groundwork.
