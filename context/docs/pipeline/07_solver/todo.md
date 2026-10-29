# Stage 07 — Solver: open work

Open work in the backends and their limits. Finished work is in [done.md](done.md).

## SOL-1 — Confirm the HiGHS time limit at the large tier

- **Done when.** The owner's large-tier run of Q2 on HiGHS returns with `TIME_LIMIT` near the budget, as medium Q2
  does at a 25 s limit ([done.md](done.md) § 2, "Time limits").

## SOL-2 — Ctrl-C during a HiGHS solve

- **Problem.** `HighsSession` inherits the no-op `SetInterruptPoll` (`solver_session.hpp`), so Ctrl-C does not stop a
  HiGHS solve; `GurobiSession` overrides it (`gurobi_solver.cpp`).
- **Change.** Override it through HiGHS's MIP, simplex and IPM interrupt callbacks, which run at its time-limit
  checkpoints.
- **Done when.** A HiGHS solve interrupted mid-run raises DuckDB's interrupt error within a second, under a test that
  sends the interrupt.
