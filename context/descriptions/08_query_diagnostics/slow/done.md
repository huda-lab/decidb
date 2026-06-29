# Query Diagnostics — Slow (how it works)

> Router terminal: **time_limit** (incumbent → report+gap · no sol → report slow). See `router/README.md`.

No slow *engine* yet — see `todo.md`. Current behavior: a solve that hits the time
limit returns `SolverStatus::TIME_LIMIT`, the router routes it to the static error
(`ThrowDecideSolveError`), and any incumbent is discarded. No interrupt mechanism
exists.

**Shared time limit (both backends).** The per-solve wall-clock cap is resolved in
one solver-agnostic place — `ResolveDecideTimeLimit()` in
`src/include/duckdb/decidb/solver_config.hpp` — so Gurobi and HiGHS honor the same
value and cannot drift. It returns `DECIDE_DEFAULT_TIME_LIMIT_SECONDS` (300s) unless
the user overrides it via the global `DECIDB_TIME_LIMIT` env var (seconds, double;
non-positive / unparseable values are ignored). Gurobi applies it as the `TimeLimit`
parameter (`gurobi_solver.cpp`); HiGHS applies it as the `time_limit` option
(`deterministic_naive.cpp`) — previously HiGHS set no limit and could run a hard MILP
indefinitely. This is the F1/S1 prerequisite that lets *both* backends actually
produce a `TIME_LIMIT` to diagnose.
