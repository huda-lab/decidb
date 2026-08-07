# Phase 5: Result Projection

## Overview

After the ILP solver returns a solution vector, `PhysicalDecide` switches from sink mode to source mode. The `GetData()` method streams results by re-scanning the materialized input data and appending decision variable values from the ILP solution.

**Key Source File**: `src/execution/operator/decide/physical_decide.cpp` (`GetData` method and `DecideGlobalSourceState`)

## Source State

`DecideGlobalSourceState` is initialized with a scan state over the materialized data (`gstate.data`) and a `current_row_offset` counter starting at 0. The operator is single-threaded (`MaxThreads() = 1`) to ensure correct sequential mapping between data rows and solution values.

## Data Flow

Each `GetData()` call:

1. Scans the next chunk of original input data from `gstate.data` into the output `DataChunk`.
2. If the chunk is empty (no more data), returns `FINISHED`.
3. For each DECIDE variable, fills in the corresponding output column with values from the ILP solution.
4. Advances `current_row_offset` by the chunk size.
5. Returns `HAVE_MORE_OUTPUT`.

The output chunk already has columns allocated for both the original data columns and the DECIDE variable columns. The DECIDE columns are the last `total_decide_vars` columns in the output, at index `types.size() - total_decide_vars + decide_var_idx`.

## Solution Mapping

The ILP solution is a flat `vector<double>` indexed by solver variable index. The `VarIndexer` maps each (DECIDE variable, row) pair to the correct solver variable index via `gstate.var_indexer.Get(var_idx, row)`. For each output row and each DECIDE variable:

```
global_row = current_row_offset + row_in_chunk
solution_idx = gstate.var_indexer.Get(decide_var_idx, global_row)
solution_value = ilp_solution[solution_idx]
```

For **row-scoped variables**, each row has its own solver variable, so distinct rows generally have distinct solution values. For **entity-scoped variables**, all rows belonging to the same entity map to the same solver variable index and therefore receive the same solution value. This is the key semantic effect of table-scoped variables: if `drivers.assigned` is declared as entity-scoped, every row for driver 42 gets the same `assigned` value.

A bounds check (`solution_idx < ilp_solution.size()`) provides a fallback of 0.0, though this should not trigger in normal operation. The `VarIndexer` used during readback is constructed via `VarIndexer::Build()` (the owning version), which copies entity mappings so it outlives the `SolverInput`.

## Type-Specific Projection

Solution values are projected with type-appropriate handling to deal with solver floating-point imprecision (e.g., a solver might return 0.9999999 instead of 1 for an integer variable):

### BOOLEAN (`BOOL`)
- Output type: `bool`
- Projection: `solution_value >= 0.5` (threshold comparison)
- A solver value of 0.9999 becomes `true`; 0.0001 becomes `false`

### INTEGER (`INT`)
- Output type: `int32_t`
- Projection: `static_cast<int32_t>(std::round(solution_value))`
- Rounding handles solver imprecision (e.g., 2.9999 becomes 3)

### BIGINT
- Output type: `int64_t`
- Projection: `static_cast<int64_t>(std::round(solution_value))`

### DOUBLE (`REAL`)
- Output type: `double`
- Projection: Direct assignment, no rounding
- Preserves the solver's exact floating-point result

### Default (fallback)
- Output type: `int64_t`
- Projection: Same as BIGINT (round and cast)

Each output vector is set to `VectorType::FLAT_VECTOR` (one value per row, no dictionary/constant compression), and values are written directly via `FlatVector::GetData<T>()`.

## Auxiliary Variable Filtering

The DECIDE variable list (`decide_variables`) includes both user-declared variables and auxiliary variables (e.g., ABS linearization auxiliaries, MIN/MAX auxiliaries). The `num_auxiliary_vars` field on `PhysicalDecide` tracks how many auxiliary variables exist.

All variables (user + auxiliary) are projected into the output chunk by `GetData()`. However, the logical plan places a projection operator above `PhysicalDecide` that filters out auxiliary variable columns, ensuring only user-declared variables appear in the final query result. The auxiliary variables occupy the last `num_auxiliary_vars` slots in the DECIDE variable list.

## Chunk-Based Output

Results are emitted in DuckDB's standard chunk-based format. The typical chunk size is `STANDARD_VECTOR_SIZE` (2048 rows). The `current_row_offset` in `DecideGlobalSourceState` ensures that each `GetData()` call maps to the correct slice of the solution vector, supporting correct pagination across multiple calls.
