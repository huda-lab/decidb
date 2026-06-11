# DeciDB: In-Database Constrained Optimization for DuckDB

## 1. Problem Statement: "Unbundling"

Standard DBMSs are excellent at retrieving and aggregating data but lack native support for combinatorial optimization. Users who need to make decisions over their data (e.g., "select a meal plan with max protein but under 2000 calories") must extract data from the database, transport it to an external solver, and re-bundle results — creating latency, consistency risks, and engineering complexity. Data management (SQL) and decision logic (OR solvers) are *unbundled*.

A key contrast with standard SQL: `WHERE` applies predicates to *individual* tuples independently, while `DECIDE` applies constraints and objectives to the *collection* of decision variables. This generalizes prior work on package queries (Brucato et al.) — which returned a subset of tuples satisfying collective constraints — to decision variables of any type, not just subset selection.

## 2. Solution: DeciDB

DeciDB extends DuckDB with native constrained optimization. A declarative `DECIDE` clause (syntax: see `syntax_reference.md`) lets users express optimization problems directly in SQL. DeciDB translates them into an Integer Linear Programming model (maximize **c**ᵀ**x** subject to A**x** ≤ **b**), solves it with an embedded solver, and returns the optimum as standard relational output. DuckDB's columnar, vectorized execution makes computing the model coefficients (**c**, A) over large datasets fast.

**Solver strategy**: **Gurobi** is the primary solver — empirically much faster on DeciDB workloads. **HiGHS** (open-source) is bundled as a fallback for environments without a Gurobi license, but it is substantially slower and not recommended for production use.

## 3. Design Goals

1. **Seamless integration**: Extend SQL syntax naturally without breaking existing functionality.
2. **Performance**: Solve directly within the database engine; minimize solver input size.
3. **Usability**: Abstract away ILP modeling and solver matrices from the database user.

For the pipeline architecture (parser → binder → planner → execution), see `../01_pipeline/architecture.md`.
