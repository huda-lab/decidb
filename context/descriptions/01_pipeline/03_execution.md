# Physical Execution & Optimization

## 1. Overview
The `PhysicalDecide` operator sits at the heart of DecidB's execution engine. It is a **blocking operator**, meaning it must consume its entire input before it can produce any output. This is necessary because the optimal value for any single decision variable depends on the entire dataset (global optimization).

**Key Source File**: `src/execution/operator/decide/physical_decide.cpp`

The execution pipeline is broken down into five sub-phases, each documented in detail in separate files (03a through 03e).

## 2. Execution Pipeline

### 2.1 Phase 1: Materialization (Sink)
DuckDB executes the query plan up to the `PhysicalDecide` node. The operator acts as a "Sink", collecting every tuple that satisfies the `WHERE` clause.
-   **Storage**: Tuples are stored in a `ColumnDataCollection` (DuckDB's efficient in-memory columnar format).
-   **Memory Management**: If the dataset exceeds memory limits, DuckDB would normally spill to disk, but the current solver integration requires in-memory access to build the matrix.

See also: materialization is standard DuckDB pipeline behavior.

### 2.1.5 Phase 1.5: Entity Mapping Construction
For table-scoped (entity-scoped) decision variables, this phase runs after data materialization but before coefficient evaluation. It evaluates entity key columns per row and builds a mapping from rows to entity IDs. This determines which rows share the same solver variable instance. See `03b_coefficient_evaluation.md` for implementation details.

### 2.2 Phase 2: Expression Analysis
Extracts the structure of constraints and objectives into `DecideConstraint` and `Objective` structs. See `03a_expression_analysis.md`.

### 2.3 Phase 3: Coefficient Evaluation
Evaluates coefficient expressions against materialized data. Computes expression-level WHEN+PER groupings, aggregate-local WHEN masks, and AVG coefficient scaling. See `03b_coefficient_evaluation.md`.

### 2.4 Phase 4: Model Building & Solving
Transforms evaluated constraints into a solver-agnostic `SolverModel` via `SolverModel::Build()`, then dispatches to Gurobi or HiGHS. See `03c_model_building.md` and `03d_solver_backends.md`.

### 2.5 Phase 5: Result Projection (Source)
Projects solution values back onto original rows with type-specific casting. See `03e_result_projection.md`.

### 2.6 EXPLAIN & Serialization
Both `LogicalDecide` and `PhysicalDecide` expose structured plan output for `EXPLAIN`, `EXPLAIN ANALYZE`, and `EXPLAIN (FORMAT JSON)`. Serialization support enables prepared statement caching. See `04_explain.md`.

## 3. Data Consistency
A critical design choice is that DecidB guarantees **read consistency**. The optimization is performed on the snapshot of data seen by the query. Any concurrent modifications to the tables do not affect the running optimization model, as it works on the materialized buffer.
