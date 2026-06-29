# Router — todo

The router is the unified post-solve dispatch (see `README.md` for the tree and
rationale). **Batch 1 shipped** the seam (`RouteSolveResult`) and the unbounded
terminal (R1 / R2); **Batch 2 shipped** residual `INF_OR_UNBD` check-ray routing
(R3); **R5 shipped** the infeasible→elastic terminal — see `done.md`. It still has
to wire the time_limit terminal to a real engine.

Tasks are individually pickable. Each carries its pointers, the decision it
settles (if any), how to test it, and which `done.md` section to write when it
lands. Suggested batches at the bottom.

---

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
- **What remains:** extend coverage when the time_limit incumbent / no-sol (R6) leaf
  lands. (The `INFEASIBLE` leaf is already unit-tested from Batch 1; R5 wired its
  engine without adding a new leaf, so it needs no further router-level test.)
- **Pointers:** `test/common/test_decidb_router.cpp` (mirror its pattern).
- **Done section:** fold into "Tests."

---

## Suggested batches

- ~~**Batch 1 (refactor, no behavior change):** R1 + R2 + R7-partial~~ — **shipped**
  (`done.md`): the seam, the unbounded terminal, and the unit-tested existing leaves.
- ~~**Batch 2 (the residual inf/unb fallback):** R3~~ — **shipped** (`done.md`):
  existing concrete-status probes stay in place; only residual `INF_OR_UNBD`
  routes through check-ray.
- ~~**R5 (infeasible→elastic terminal):**~~ — **shipped** (`done.md`): the
  `INFEASIBLE` arm runs the elastic engine.
- **Batch 3 (blocked, lands with its engine):** R6 with the slow groundwork.
