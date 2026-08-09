# Phase 2: Coefficient Evaluation

## Overview

After expression analysis extracts the *structure* of constraints and objectives (which variable, which coefficient expression), this phase *evaluates* those coefficient expressions against the materialized data to produce concrete numeric values. This happens in the `Finalize()` method of `PhysicalDecide`.

The input to this phase is a set of `DecideConstraint` and `Objective` structs (from Phase 1) containing unevaluated expression trees. The output is `EvaluatedConstraint` structs and objective coefficient arrays containing concrete `double` values for every (term, row) pair.

**Key Source File**: `src/execution/operator/decide/physical_decide.cpp` (`Finalize()` method)

## Expression Transformation

Before evaluation, `BoundColumnRefExpression` nodes (which reference columns by table binding) must be converted to `BoundReferenceExpression` nodes (which reference columns by chunk index). This is necessary because DuckDB's `ExpressionExecutor` works with column indices within a `DataChunk`, not with table-level column bindings.

The `TransformExpression` lambda performs this recursively:
- `BoundColumnRefExpression` -> `BoundReferenceExpression` using `colref.binding.column_index`
- `BoundFunctionExpression` -> recurse into children, copy `bind_info`
- `BoundCastExpression` -> recurse into child, re-wrap with `AddCastToType`
- `BoundAggregateExpression` -> special case: `count_star()` is replaced with a `BoundConstantExpression(num_rows)`; other direct RHS aggregates are rejected with `InvalidInputException` (use a scalar subquery for aggregate bounds)
- Constants and other expressions -> copied as-is

This lambda is defined multiple times (for LHS coefficients, RHS expressions, WHEN conditions, PER columns, and objective terms) with slight variations in which expression types are handled.

### Precondition: every evaluated expression must already be resolved

The `BoundColumnRefExpression` branch reads `colref.binding.column_index` and uses it **as a position in the materialized chunk**. Those two indexings only agree because `ColumnBindingResolver` has already rewritten the DECIDE clause's column references to `BoundReferenceExpression`s carrying chunk positions — the leftover `BoundColumnRef` case is a fallback, not the normal path.

That makes resolution a hard precondition of this phase, and it is enforced by hand: the resolver's `LOGICAL_DECIDE` case (`src/execution/column_binding_resolver.cpp`) enumerates DECIDE's expression-holding fields explicitly rather than using `VisitOperatorExpressions`, because DECIDE variables must be shielded from resolution through `ignored_bindings`. **Any new field on `LogicalDecide` that holds expressions this phase will evaluate must be added to that enumeration.** An optimizer rewrite that *moves* sub-expressions out of `decide_objective` / `decide_constraints` into a side vector — as the composed MIN/MAX rewrite does — silently opts them out otherwise, and the operator then evaluates a logical index as a chunk position. A single-table source hides the mistake completely, because the two indexings coincide there; it surfaces only once a join reorders the columns. Currently enumerated: `decide_variables`, `decide_constraints`, `decide_objective`, `composed_minmax_objective_terms`, `composed_minmax_constraints`.

`entity_key_expressions` is deliberately **not** resolved: `plan_decide.cpp` matches those against the child's `GetColumnBindings()` by `(table_index, column_index)` to compute `entity_key_physical_indices`, which needs the logical binding intact.

## Per-Term Coefficient Evaluation

For each constraint, each term's coefficient expression is evaluated against the materialized data:

1. The coefficient expression is transformed via `TransformExpression`.
2. An `ExpressionExecutor` is created and the transformed expression is added.
3. Data is scanned chunk-by-chunk from `gstate.data` (the materialized input).
4. For each chunk, the executor produces a result vector, and values are extracted row-by-row.
5. Each value is cast to `double` via `DefaultCastAs(LogicalType::DOUBLE)`.
6. Results accumulate into `row_coefficients[term_idx]`, a vector of doubles indexed by global row.

The term's `variable_index` (which DECIDE variable it multiplies) is preserved from Phase 1. Data-only LHS terms use `INVALID_INDEX`; for aggregate constraints those fixed coefficients are still evaluated for each active row, then the model builder subtracts their row/group sum from the scalar RHS.

## RHS Evaluation

The right-hand side of each constraint is evaluated similarly:

- **Constant RHS** (`BoundConstantExpression`): The constant value is extracted once and broadcast to all rows via `assign(num_rows, rhs_constant)`.
- **Expression RHS** (column reference, function, etc.): Evaluated per-row via `ExpressionExecutor` against data chunks, producing one RHS value per row. This handles row-varying bounds like `x <= column_value`.

Results are stored in `rhs_values[row_idx]`.

## Expression-level WHEN+PER Row-to-Group Mapping

After coefficient evaluation, the WHEN and PER modifiers are processed to compute `row_group_ids` -- a per-row vector that assigns each row to a constraint group (or excludes it). There are 4 code paths:

### Neither WHEN nor PER
`row_group_ids` stays empty, `num_groups = 0`. This is the fast path -- the model builder treats all rows as one implicit group.

### WHEN Only (no PER)
The WHEN condition expression is transformed and evaluated per-row to produce a boolean mask. Rows where the condition is true (or non-NULL) get group ID `0`; rows where it is false or NULL get `INVALID_INDEX` (excluded). `num_groups = 1`.

### PER Only (no WHEN)
The PER column expression is evaluated per-row. Distinct values are mapped to group IDs via a `string`-keyed hash map (`unordered_map<string, idx_t>`), assigned in first-seen order. NULL PER values get `INVALID_INDEX` (excluded, matching SQL GROUP BY NULL semantics). `num_groups = K` (number of distinct non-NULL values).

### WHEN + PER
Both are evaluated. WHEN filtering is applied first: rows not matching the WHEN condition get `INVALID_INDEX`. Among surviving rows, PER grouping assigns group IDs as above. `num_groups = K`.

## Aggregate-local WHEN Masks

Aggregate-local `WHEN` filters are carried on individual extracted terms rather than on the whole `DecideConstraint` or `Objective`. During `Finalize()`:

1. Each distinct term-level filter expression is transformed and evaluated into a boolean row mask.
2. For linear, bilinear, and quadratic aggregate terms, rows where the local filter is false or NULL get coefficient `0` for that term only.
3. If PER grouping is present, a row is assigned to a group only if it passes the expression-level WHEN (if any), has a non-NULL PER key, and participates in at least one aggregate-local term.
4. Rows outside a term's local filter are not removed from unrelated constraints or objective terms.

This lets one constraint or objective use independent aggregate filters:

```sql
SUM(x * a) WHEN active + SUM(x * b) WHEN priority <= 10 PER grp
```

Here `active` only filters the `a` term, `priority` only filters the `b` term, and `PER grp` groups rows that participate in at least one local term.

## NULL/NaN/Infinity Validation

After evaluation, every coefficient and RHS value is checked:

- **NULL values**: Cause an `InvalidInputException` with a message suggesting `COALESCE()` or `WHERE` filtering.
- **NaN or Infinity**: Cause an `InvalidInputException` listing common causes (division by zero, arithmetic overflow, NULL propagation through math operations).

These checks apply to both constraint coefficients and objective coefficients.

## Objective Coefficient Evaluation

The objective follows the same pattern as constraints:

1. Each term's coefficient expression is transformed and evaluated chunk-by-chunk.
2. Results go into `evaluated_objective_coefficients[term_idx][row_idx]`.
3. Variable indices are stored in `objective_variable_indices[term_idx]`.

If the objective has a WHEN condition, it is evaluated as a boolean mask. Rows where the condition is false or NULL have all their objective coefficients zeroed out (effectively excluding them from the objective sum).

If objective terms have aggregate-local filters, those filters are applied per term before the expression-level objective WHEN is applied. Objective PER grouping uses the same row-participation rule as constraints: a row must pass the expression-level objective WHEN (if present) and at least one local aggregate filter.

## AVG Scaling

`AVG` is rewritten to `SUM` by the optimizer and tagged with `AVG_REWRITE_TAG`. During coefficient evaluation, terms marked with `avg_scale` are scaled by the number of active rows in the applicable group:

- Ungrouped: scale by total active row count
- Expression-level WHEN: scale by rows passing the WHEN mask
- PER: scale by group size
- Aggregate-local WHEN: scale by rows passing that aggregate-local filter, within the group if PER is present

Quadratic AVG terms scale their inner linear coefficients by `1/sqrt(N)` so the resulting quadratic outer product is scaled by `1/N`.

## Entity Mapping Construction (Phase 1.5)

For table-scoped (entity-scoped) decision variables, entity mappings are constructed after data materialization but before coefficient evaluation. This determines how rows map to shared solver variable instances.

### Construction Process

For each `EntityScopeInfo` on the `PhysicalDecide` operator:

1. **Key column evaluation**: The entity key columns (identified by `entity_key_physical_indices`) are evaluated per row from the materialized data.
2. **Composite key hashing**: For each row, the key column values are concatenated into a composite string key with NULL-safe tagging (to distinguish NULL from the string "NULL"). This uses the same pattern as PER grouping.
3. **Entity ID assignment**: An `unordered_map<string, idx_t>` maps each unique composite key to an entity ID, assigned in first-seen order.
4. **Row-to-entity vector**: A `row_to_entity` vector of size `num_rows` is populated, mapping each row to its entity ID.

### EntityMapping Struct

The result is stored in an `EntityMapping` struct (defined in `solver_input.hpp`):
- `num_entities`: The count of distinct entities found
- `row_to_entity`: Vector mapping each row index to its entity ID

This mapping is stored on the `SolverInput` and used by the model builder to determine variable indexing for entity-scoped variables.

## Output

The evaluated data is packaged into a `SolverInput` struct:

- `num_rows`, `num_decide_vars`: Dimensions
- `variable_types[var]`: Logical type of each DECIDE variable — reported as `LogicalType::BOOLEAN` whenever `is_boolean_var[var]` is true, even if the variable's DuckDB-facing type is INTEGER (user-declared `BOOL`, IN-domain/L0 auxiliaries), so the model builder applies the `[0,1]` box and `is_binary` from the type alone
- `lower_bounds[var]`, `upper_bounds[var]`: Bounds from `TraverseBoundsConstraints()` (genuine user-written constraints only; intersected with type defaults, including the BOOLEAN `[0,1]` default above)
- `constraints`: Vector of `EvaluatedConstraint`, each containing:
  - `variable_indices[term_idx]`: Which variable each term references
  - `row_coefficients[term_idx][row_idx]`: Numeric coefficient values
  - `rhs_values[row_idx]`: Right-hand side values
  - `comparison_type`: The comparison operator
  - `row_group_ids[row_idx]`: Group assignment (empty for ungrouped)
  - `num_groups`: Number of distinct groups
  - `lhs_is_aggregate`: Flag for model builder
- `objective_coefficients[term_idx][row_idx]`: Objective term values
- `objective_variable_indices[term_idx]`: Which variable each objective term references
- `sense`: MAXIMIZE or MINIMIZE

When the objective is quadratic (`has_quadratic = true`), the same evaluation runs on `squared_terms` **in addition to** `terms` (not instead of): `EvaluateObjectiveTermList` is called once per non-empty list, populating `objective_coefficients` / `objective_variable_indices` for the linear half and `quadratic_inner_coefficients` / `quadratic_variable_indices` for the quadratic half. Pure-linear objectives populate only the linear arrays, pure-quadratic objectives populate only the quadratic arrays, and mixed objectives (e.g. `SUM(POWER(x - t, 2) + c * x)`) populate both. The expression-level WHEN mask and aggregate-local per-term filters are applied independently to each list.

This `SolverInput` is then passed to `SolveModel()`, which hands it to the model builder (Phase 3) and solver backend (Phase 4).
