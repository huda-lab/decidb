# Theoretical Background

## 1. The "Unbundling" Problem
In traditional data analysis pipelines involving optimization, there is a fundamental disconnect known as "Unbundling".
-   **Data Management**: Handled by DBMS (SQL). Efficient at storage, retrieval, filtering.
-   **Decision Logic**: Handled by Solvers (OR tools). Efficient at searching solution spaces.
-   **The Gap**: To solve a problem, data must be unbundled from the database, transported to the application, and re-bundled into the solver's format. This creates latency, consistency risks, and engineering complexity.

## 2. Constrained Optimization over Relations
DeciDB expresses *decision problems* directly in SQL: the `DECIDE` clause declares decision variables over the rows of a query, and the solver assigns their values to satisfy global constraints and optimize an objective — a constrained optimization problem (COP).
-   **Definition**: A query that assigns values to decision variables (e.g., whether to keep each tuple, or what value to synthesize) so that the *collection* of choices satisfies global constraints.
-   **Contrast**: Standard SQL `WHERE` clauses apply predicates to *individual* tuples independently. `DECIDE` clauses apply constraints and objectives to the *collection* of decision variables.
-   **Relation to prior work**: This generalizes earlier work on package queries (Brucato et al.), which returned a *package* — a subset of tuples satisfying collective constraints — to support general decision variables of any type, not just subset selection.

## 3. Integer Linear Programming (ILP)
DeciDB maps these decision problems to ILP problems.
-   **Canonical Form**:
    $$ \text{maximize } \mathbf{c}^T \mathbf{x} $$
    $$ \text{subject to } A \mathbf{x} \le \mathbf{b} $$
    $$ \mathbf{x} \in \mathbb{Z}^n $$
-   **Mapping**:
    -   $\mathbf{x}$: The decision variables (one per row).
    -   $\mathbf{c}$: The coefficients of the objective function (derived from row values).
    -   $A, \mathbf{b}$: The constraint matrix and bounds.

## 4. Why DuckDB + Gurobi/HiGHS?
-   **DuckDB**: Columnar, vectorized execution makes it fast to compute the coefficients ($\mathbf{c}$ and $A$) over large datasets.
-   **Gurobi**: A state-of-the-art commercial solver with excellent performance on large-scale mixed-integer problems. Used as the primary solver when a license is available.
-   **HiGHS**: A modern, open-source C++ solver bundled as the fallback. Embedding it creates a zero-copy (or near zero-copy) path from DB memory to Solver memory, ensuring DeciDB works out of the box without any commercial dependencies.
