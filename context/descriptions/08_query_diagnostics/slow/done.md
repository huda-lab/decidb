# Query Diagnostics — Slow (how it works)

> Router terminal: **time_limit** (incumbent → report+gap · no sol → report slow). See `router/README.md`.

Nothing implemented yet — see `todo.md`. Current behavior: Gurobi hits
`DECIDB_TIME_LIMIT` (300s default) and throws, discarding any incumbent
(`gurobi_solver.cpp`); HiGHS sets no time limit. No interrupt mechanism exists.
