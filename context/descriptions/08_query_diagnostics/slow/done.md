# Query Diagnostics — Slow (implemented)

Nothing implemented yet — see `todo.md`. Current behavior: Gurobi hits
`DECIDB_TIME_LIMIT` (300s default) and throws, discarding any incumbent
(`gurobi_solver.cpp:254-259`); HiGHS sets no time limit. No interrupt mechanism
exists.
