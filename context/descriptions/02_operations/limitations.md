# Limitations & Future Work

## 1. Current Limitations

While DecidB demonstrates the feasibility of in-database optimization, the current implementation has specific constraints:

### 1.1 Constraint and Objective Forms
DecidB supports **LP**, **ILP**, **MILP**, **QP**, **MIQP**, **QCQP**, bilinear, and feasibility problem classes (see `03_expressivity/problem_types/done.md` for the full taxonomy).
-   **Constraints** may be linear ($\sum c_i x_i \le K$), bilinear (`x * y`), or quadratic (`POWER(expr, 2)`). Bilinear and quadratic constraints require Gurobi (HiGHS rejects).
-   **Objectives** may be linear ($\max \sum c_i x_i$), quadratic (`MINIMIZE SUM(POWER(linear_expr, 2))`), bilinear, or omitted (feasibility).
-   **Remaining limitation**: Higher-order nonlinearities (cubic, etc.) are not supported — only linear, bilinear, and degree-2 quadratic forms are allowed.

### 1.2 Scalability (Exact Solver)
Both Gurobi and HiGHS are exact solvers. For NP-Hard problems (integer optimization), the run-time can grow exponentially with the number of decision variables (rows).
-   **Benchmark data** (HiGHS, commit 6b56b35): ~500 rows solves in ~60ms, ~2000 rows in ~290ms, ~10000 rows in ~1450ms (solver time only, simple binary knapsack).
-   **Current Limit**: Effective for thousands of rows (HiGHS) to tens of thousands (Gurobi).
-   **Bottleneck**: For millions of rows, an exact IP solver will time out. Solver time dominates at ~95% of total execution time at scale (see `02_operations/benchmarking.md`).

### 1.3 Table-Scoped Variable Entity Keys
Table-scoped variables (`DECIDE Table.var`) identify entities using all columns from the source table as a composite key. There is no syntax to specify a custom key subset (e.g., primary key only). For tables with many columns, this may create unnecessarily wide composite keys. Entity identification relies on exact value matching across all columns; if two rows differ in any column (even non-key columns), they are treated as distinct entities.

## 2. Future Work

### 2.1 Approximate Solvers (Heuristics)
To handle "Big Data" optimization (millions of rows), future versions could integrate heuristic solvers (e.g., Greedy approaches, Simulated Annealing) or relaxation-based approaches (solving the LP relaxation and rounding).
-   **Benefit**: Near-optimal solutions in linear time.
-   **Trade-off**: No guarantee of global optimality.

### 2.2 Incremental Optimization
Currently, the entire dataset is materialized. Future work could explore "Interactive Optimization," where the user provides feedback on a solution, and the solver incrementally updates the result without re-building the entire model.

## 3. Completed (Previously Future Work)

### ~~3.1 Conditional Expressions (`WHEN` Keyword)~~
Implemented. See `context/descriptions/03_expressivity/when/done.md`.

### ~~3.2 Gurobi Solver Integration~~
Implemented. See `context/descriptions/01_pipeline/03d_solver_backends.md`.

### ~~3.3 Correlated Subquery Unnesting~~
Implemented. Correlated subqueries in `SUCH THAT` constraints are unnested into joins before the DECIDE clause. See `test/decide/tests/test_cons_correlated_subquery.py`.
