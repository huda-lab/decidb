# Query Diagnostics — Foundations (implemented)

Nothing in this area is implemented yet — these are the foundations to build (see
`todo.md`). Verified current baseline the foundations build on:

- **Solver result is a bare `vector<double>`** (`src/include/duckdb/decidb/ilp_solver.hpp:29`);
  both backends throw on non-optimal status (`gurobi_solver.cpp:227`,
  `deterministic_naive.cpp:207`). F1 replaces this.
- **`ModelConstraint` has no provenance** — only indices / coefficients / sense /
  rhs (`src/include/duckdb/decidb/ilp_model.hpp:78-83`). F2 adds it.
- **Gurobi reads `DECIDB_TIME_LIMIT`** (300s default, `gurobi_solver.cpp:70-81`);
  **HiGHS sets no time limit**. F1 wires HiGHS.
