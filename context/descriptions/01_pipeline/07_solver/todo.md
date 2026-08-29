# Stage 07 — Solver: open work

No solver-stage work is open. Shipped behavior is recorded in `done.md`.

`SolverConstructSupport::bilinear` (`src/include/duckdb/common/decide_solver_capabilities.hpp`)
is always `false` today. This is intentional, not a bug: it stays `false`
until a backend's loader binds the symbols it needs and stage 08 knows how
to emit a bilinear construct through it. Noted here so a permanently-false
flag with no reader doesn't get mistaken for dead code.
