# Stage 07 — Solver: open work

- Gurobi's native multi-objective API for `THEN` stages (today: staged solves on both backends).

`SolverConstructSupport::bilinear` (`src/include/duckdb/common/decide_solver_capabilities.hpp`)
is always `false` today. This is intentional, not a bug: it stays `false`
until a backend's loader binds the symbols it needs and stage 08 knows how
to emit a bilinear construct through it. Noted here so a permanently-false
flag with no reader doesn't get mistaken for dead code.
