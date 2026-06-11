# System Architecture

## 1. High-Level Design

DeciDB is implemented as an extension to DuckDB. It leverages DuckDB's extensible parser and planner architecture to inject new query operators. The system follows a pipelined execution model where the `DECIDE` clause is treated as a specialized aggregation step that occurs after standard filtering and before final projection.

For the component-by-component breakdown (symbolic layer, binders, DecideOptimizer, PhysicalDecide, solver backends) and file map, see `code_structure.md`. For a concrete end-to-end example, see `trace_life_of_a_query.md`.

## 2. Query Lifecycle

The execution of a decision query follows these stages:

```mermaid
graph TD
    User[User SQL Query] --> Parser
    subgraph DuckDB + DeciDB Extension
    Parser[Parser (DuckDB + Symbolic Layer)] --> Binder
    Binder[Binder (Validation & Types)] --> Planner
    Planner[Logical Planner] --> Opt[Optimizer]
    Opt --> Phys[Physical Planner]
    Phys --> Exec[Execution Engine]
    
    subgraph DeciDB Execution
        Exec --> Data[Materialize Candidates]
        Data --> Matrix[Build Solver Matrix]
        Matrix --> Model[SolverModel Builder]
        Model --> Solver[Solver: Gurobi (primary) / HiGHS (slow fallback)]
        Solver --> Result[Map Solution to Rows]
    end
    end
    Result --> Output[Result Table]
```

## 3. Detailed Data Flow

### 3.1 Parsing & Normalization
When the parser encounters a `DECIDE` clause, it invokes the Symbolic Layer. This layer:
1.  Identifies decision variables.
2.  Normalizes constraints into the form `SUM(coeff * variable) <= constant`.
3.  Separates row-varying coefficients (dependent on table columns) from decision variables.

### 3.2 Plan Generation
The planner inserts a `LogicalDecide` operator into the query tree. Crucially, this operator is placed **above** the source-table scan and `Filter` operators. This ensures that the solver only considers rows that satisfy the `WHERE` clause, significantly reducing the problem size.

### 3.3 Physical Execution
The `PhysicalDecide` operator works in a "stop-and-go" fashion (pipeline breaker):
1.  **Sink Phase**: It consumes all input tuples from its child operator (the candidate items). These tuples are buffered in memory.
2.  **Entity Mapping (Phase 1.5)**: For table-scoped (entity-scoped) variables, the operator evaluates entity key columns per row and builds a row-to-entity mapping. This determines which rows share the same solver variable instance.
3.  **Model Building**: It iterates over the buffered tuples to compute the coefficients for the objective function and constraints. The `VarIndexer` computes a three-block variable layout: row-scoped variables (one per row), entity-scoped variables (one per unique entity), and global auxiliary variables. The indexer replaces the previous flat `row * num_vars + var_idx` formula with scope-aware indexing.
4.  **Solve Phase**: The constructed model is passed to `SolverModel::Build()` which creates a solver-agnostic representation, then `SolveModel()` dispatches to Gurobi (primary, significantly faster in practice) or HiGHS (slow fallback if Gurobi is unavailable).
5.  **Source Phase**: Once the solver returns, the operator augments the buffered tuples with the solution values (e.g., `x=1` or `x=0`) and streams them to the next operator (e.g., `SELECT` list projection). For entity-scoped variables, all rows belonging to the same entity receive the same solution value via `VarIndexer::Get(var_idx, row)`.

> **Note**: The execution phase is documented in detail across five sub-documents: expression analysis (03a), coefficient evaluation (03b), model building (03c), solver backends (03d), and result projection (03e). See each for implementation details.

## 4. Integration Point
DeciDB links against DuckDB as a loadable extension. It registers:
-   New Parser Keywords: `DECIDE`, `SUCH THAT`, `MAXIMIZE`, `MINIMIZE`.
-   New Transformer Rules: To convert parsed nodes into logical operators.
-   New Physical Operator: `PhysicalDecide`.
-   EXPLAIN Support: Both `LogicalDecide` and `PhysicalDecide` override `GetName()` and `ParamsToString()` to produce structured DECIDE node output in `EXPLAIN`, `EXPLAIN ANALYZE`, and `EXPLAIN (FORMAT JSON)`. See `01_pipeline/04_explain.md` for details.
