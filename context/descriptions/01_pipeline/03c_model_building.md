# Phase 3: Model Building

## Overview

The model builder transforms the solver-agnostic `SolverInput` (evaluated constraints with numeric coefficients) into a `SolverModel` (flat variable arrays + constraint list in COO format + optional Q matrix for QP). This is the bridge between DecidB's domain model and the generic optimization formulation that any solver backend can consume.

The core logic lives in `SolverModel::Build()`, a static method that takes a `SolverInput` (by non-const reference, so raw global constraints can be moved rather than copied) and a pre-built `VarIndexer`, and returns a fully constructed `SolverModel`. The `VarIndexer` is constructed once in `PhysicalDecide::Finalize()` and threaded through both `SolveModel()` and `Build()` to avoid duplicate construction (see `01_pipeline/03_execution.md`).

**Key Source File**: `src/decidb/utility/ilp_model_builder.cpp`
**Headers**: `src/include/duckdb/decidb/ilp_model.hpp`, `src/include/duckdb/decidb/solver_input.hpp`

## Variable Setup

### VarIndexer: Three-Block Layout

The `VarIndexer` (defined in `ilp_model.hpp`) computes the total number of solver variables and maps each (DECIDE variable, row) pair to a solver variable index. It uses a three-block layout:

```
Block 1: Row-scoped variables         [0, num_rows * num_row_vars)
Block 2: Entity-scoped variables       [block1_end, block1_end + sum(num_entities_per_var))
Block 3: Global auxiliary variables    [block2_end, block2_end + num_aux_globals)
```

**Block 1 (Row-scoped)**: Standard DECIDE variables that get one solver variable per row. Indexed as `row * num_row_vars + row_var_offset[var_idx]`. This is the traditional layout — equivalent to the old `row * num_decide_vars + var_idx` when all variables are row-scoped.

**Block 2 (Entity-scoped)**: Table-scoped variables that share a solver variable across rows belonging to the same entity. Each entity-scoped variable gets `num_entities` solver variables (one per unique entity). Indexed as `entity_var_base[var_idx] + entity_id`, where `entity_id` comes from the `EntityMapping::row_to_entity` vector.

**Block 3 (Global auxiliaries)**: Auxiliary variables that exist once globally (ABS linearization auxiliaries, MIN/MAX auxiliary variables). These receive `INVALID_INDEX` as their scope marker during optimizer processing, signaling row-scoped treatment.

The `VarIndexer` provides two key methods:
- **`VarIndexer::Get(var_idx, row)`**: Returns the solver variable index for a given DECIDE variable and row. For row-scoped variables, this computes `row * num_row_vars + offset`. For entity-scoped variables, this looks up `row_to_entity[row]` and returns `entity_var_base[var_idx] + entity_id`.
- **`VarIndexer::NumInstances(var_idx)`**: Returns the number of solver variable instances for a given DECIDE variable — `num_rows` for row-scoped, `num_entities` for entity-scoped.

The indexer is constructed via two static methods:
- **`VarIndexer::Build()`**: Owning construction — copies entity mappings so the VarIndexer can outlive the `SolverInput`. This is the form used in `PhysicalDecide::Finalize()`: the indexer is built once early in finalization, threaded through `SolveModel()` / `SolverModel::Build()`, and finally moved onto `gstate.var_indexer` for solution readback in `GetData`. `total_vars` is refreshed just before the solve once all auxiliary globals have been appended.
- **`VarIndexer::BuildRef()`**: Lightweight construction — stores a `const` pointer to the `SolverInput`'s entity mappings without copying. Available for callers that need a short-lived non-owning indexer (currently no production callers; retained for tests and future use).

### Per-Variable Type and Default Bounds

Each DECIDE variable's logical type determines its solver properties:

| Logical Type | `is_binary` | `is_integer` | Default Bounds |
|---|---|---|---|
| BOOLEAN | true | true | [0, 1] |
| INTEGER / BIGINT | false | true | [0, 1e30] |
| DOUBLE / FLOAT (REAL) | false | false | [0, 1e30] |

### Explicit Bounds Intersection

Explicit bounds from `SolverInput` (extracted from simple constraints like `x >= 5`, `x <= 10` during Phase 1) are intersected with the type-based defaults:

- Lower bound: `max(type_default_lower, explicit_lower)`
- Upper bound: `min(type_default_upper, explicit_upper)`

### Expansion to Solver Variables

Bounds and type properties are assigned per solver variable instance. For row-scoped variables, every row gets the same bounds and type for a given DECIDE variable. For entity-scoped variables, each entity instance gets the same bounds and type — the number of instances is `NumInstances(var_idx)` rather than `num_rows`.

## Objective Coefficient Array

The objective is represented as a flat `obj_coeffs` vector of size `total_vars`, initialized to 0.0. For each objective term:

- `objective_variable_indices[term_idx]` identifies the DECIDE variable.
- `objective_coefficients[term_idx][row]` provides the per-row coefficient.
- Each coefficient is placed at the solver variable index returned by `var_indexer.Get(decide_var_idx, row)`.

For entity-scoped variables, multiple rows may map to the same solver variable. When this happens, the coefficients from all rows sharing the same entity are **accumulated** (summed) into the same `obj_coeffs` slot. This correctly models the fact that one entity variable participates in the objective across all its rows.

The `maximize` flag is set from `input.sense`.

## Constraint Building

Each `EvaluatedConstraint` is converted to one or more `ModelConstraint` structs. The path depends on whether the constraint is aggregate (SUM-level) and whether it has groups (WHEN/PER).

### Row-Scoped Fast Path (Path 1 and Path 2)

`CanUseRowScopedFastPath()` inspects the constraint's `variable_indices` once up front and returns true only when (a) every term references a row-scoped decide variable and (b) no decide variable appears in more than one term. Under those conditions each `(term, row)` pair produces a unique flat solver index, so coefficients can be pushed directly into `constr.indices` / `constr.coefficients` without sparse accumulation. Entity-scoped vars or duplicated decide vars fall back to the accumulation path (`SparseCoeffAccumulator`, declared in `ilp_model.hpp`), which the aggregate paths need to handle `(entity, row)` collisions correctly. The accumulator picks one strategy per constraint and reuses scratch storage across all groups: a dense `vector<double>` indexed by flat var idx with a `touched` list (when the decide-variable index span fits in the cap, ~1M slots), or a sorted/merged `(idx, coeff)` pair list otherwise.

Both the ungrouped and grouped aggregate branches use this same fast-path selector.

### Path 1: Aggregate, Ungrouped (No WHEN, No PER)

A single `ModelConstraint` is produced that sums over all rows:

- **Fast path (all-row-scoped, no duplicate vars)**: reserve `active_terms * num_rows` capacity and push each `(term, row)` contribution directly, skipping zeros.
- **Accumulation path**: `SparseCoeffAccumulator` keyed by solver variable index, then flushed to COO format. Needed when entity-scoped variables or repeated decide vars can collide on the same flat index.
- RHS comes from `rhs_values[0]`.
- AVG terms have already been coefficient-scaled in Phase 2. The model builder consumes the evaluated coefficients directly.

### Path 2: Aggregate, Grouped (WHEN and/or PER)

A `group_to_rows` index is built: for each group ID, collect which rows belong to it (skipping `INVALID_INDEX` rows). Then one `ModelConstraint` is emitted per non-empty group, using the same fast-path vs. accumulation split as Path 1 (reserves `active_terms * group_rows[g].size()` on the fast path).

- Only rows in the group contribute coefficients.
- RHS comes from `rhs_values[0]`.
- AVG terms have already been coefficient-scaled in Phase 2 using the group's active row count. The model builder consumes the evaluated coefficients directly.

### Path 3: Per-Row

One `ModelConstraint` per row. Rows with `row_group_ids[row] == INVALID_INDEX` (excluded by WHEN) are skipped. Each constraint pre-reserves `per_row_active_terms` and contains only the terms for that single row, with RHS from `rhs_values[row]`. Variable indices are obtained via `var_indexer.Get(decide_var_idx, row)`.

### Normalization on Emission

All three paths push through `PushNormalizedConstraint()` rather than `model.constraints.push_back()` directly. It drops empty-LHS tautologies (`0 <= k>=0`, `0 >= k<=0`, `0 = 0`) and throws `InvalidInputException` for violated empty-LHS rows (e.g., `0 <= -1` after absorbing variable bounds and cancelling terms) — surfacing infeasibility here yields a clearer error than a solver status code.

### Size Estimation and Reservation

Before the constraint loop, the builder sums an upper bound on `model.constraints.size()` (aggregates contribute 1 or `num_groups`, per-row contributes `num_rows`) and reserves once. `deterministic_naive.cpp` precomputes total nnz across `model.constraints` and reserves `a_rows` / `a_cols` / `a_vals` / `row_lower` / `row_upper` in one shot, avoiding repeated reallocation when flattening to HiGHS COO.

## `ApplyComparisonSense()`

Converts DuckDB's `ExpressionType` comparisons to ILP constraint sense characters:

| ExpressionType | Sense Char | RHS Treatment |
|---|---|---|
| `COMPARE_GREATERTHANOREQUALTO` | `'>'` (meaning >=) | Direct |
| `COMPARE_GREATERTHAN` | `'>'` | `floor(rhs) + 1.0` (strict to non-strict for integers) |
| `COMPARE_LESSTHANOREQUALTO` | `'<'` (meaning <=) | Direct |
| `COMPARE_LESSTHAN` | `'<'` | `ceil(rhs) - 1.0` (strict to non-strict for integers) |
| `COMPARE_EQUAL` | `'='` | Direct |

Note: The sense characters `'>'` and `'<'` represent `>=` and `<=` respectively (standard ILP convention). Strict inequalities are converted by adjusting the RHS, which is exact for integer variables but an approximation for continuous variables.

## Quadratic Objective (Q Matrix)

When `input.has_quadratic_objective` is true, the model builder constructs the Q matrix for the standard QP form `minimize (1/2) x^T Q x + c^T x`.

The inner linear expression of `SUM(POWER(expr, 2))` has already been evaluated per-row in `SolverInput::quadratic_inner_coefficients`. For each row, the inner expression is `sum_t(a_{t,row} * x_{var_t})`. The Q matrix is built by summing the outer products across all rows:

1. **Variable terms**: For each row, collect non-zero coefficients for each DECIDE variable. Build Q entries from the outer product: `Q[i,j] += 2 * a_i * a_j` (factor of 2 on all entries — both diagonal and off-diagonal — due to the `(1/2) x^T Q x` convention).

2. **Constant terms**: If the inner expression has a constant part `c`, the expansion `(expr + c)^2 = expr^2 + 2c·expr + c^2` produces linear cross-terms `2c·a_t` that are added to `obj_coeffs`.

3. **Storage**: Q is accumulated in a `std::map<pair<int,int>, double>` (lower triangle, row >= col) then serialized to COO vectors (`q_rows`, `q_cols`, `q_vals`).

**Convexity guarantee**: Because Q = sum(a·a^T) = A^T A, it is always positive semidefinite by construction. This is the key payoff of the syntax-enforced convexity design.

## `ModelConstraint` Format

Each constraint is stored in COO (coordinate) format:

```
struct ModelConstraint {
    vector<int> indices;        // Variable indices into the flattened array
    vector<double> coefficients; // Coefficient for each variable
    char sense;                 // '<' (<=), '>' (>=), '=' (==)
    double rhs;                 // Right-hand side value
};
```

This format is consumed directly by Gurobi (`GRBaddconstr` takes COO) and converted to CSR for HiGHS.

## `SolverModel` Structure

```
struct SolverModel {
    idx_t num_vars;              // total_vars from VarIndexer (three-block layout)
    vector<double> col_lower;    // Lower bounds per solver variable
    vector<double> col_upper;    // Upper bounds per solver variable
    vector<bool> is_integer;     // True for INTEGER/BOOLEAN, false for REAL
    vector<bool> is_binary;      // True for BOOLEAN (subset of integer)
    vector<double> obj_coeffs;   // Linear objective coefficient per variable
    bool maximize;               // True = maximize, false = minimize
    vector<int> q_rows;          // Q matrix row indices (COO, lower triangle)
    vector<int> q_cols;          // Q matrix column indices (COO, lower triangle)
    vector<double> q_vals;       // Q matrix values
    bool has_quadratic_obj;      // True if QP objective present
    vector<ModelConstraint> constraints;
};
```

## Sanity Checks

After building, the model is validated:

1. **Bounds**: Every variable must have finite lower and upper bounds, with `lower <= upper`. Violation throws `InternalException` with the column index and bound values.

2. **Objective coefficients**: Every objective coefficient must be finite (not NaN or Infinity).

3. **Constraint validity**: For each constraint:
   - `indices` and `coefficients` vectors must be the same size.
   - Every variable index must be within `[0, total_vars)`.
   - Every coefficient must be finite.
   - RHS must not be NaN (infinity is allowed for range-based representations).

These checks catch bugs in the evaluation or model-building logic before they propagate to the solver, where error messages would be less informative.
