# DECIDE Clause — Implemented Features

The `DECIDE` clause declares **decision variables** of a COP query. Each variable gets a value assigned by the solver for every row in the input relation, and the assigned values appear as new columns in the query result.

---

## Syntax

```sql
DECIDE [Table.]variable_name [IS type] [, [Table.]variable_name2 [IS type2] ...]
```

Variables are declared as a comma-separated list with optional type annotations.

---

## Variable Types

### `IS BOOLEAN`

The variable takes values in {0, 1}. DecidB automatically adds lower bound (`x >= 0`) and upper bound (`x <= 1`) constraints.

```sql
DECIDE x IS BOOLEAN
```

### `IS INTEGER`

The variable takes non-negative integer values {0, 1, 2, ...}. This is the **default** if no type is specified.

```sql
DECIDE x IS INTEGER
DECIDE x              -- same as IS INTEGER
```

### `IS REAL`

The variable takes non-negative continuous (floating-point) values in `[0, +inf)`. Internally stored as `LogicalType::DOUBLE`. Both HiGHS (`kContinuous`) and Gurobi (`GRB_CONTINUOUS`) natively support continuous variables.

```sql
DECIDE x IS REAL
```

REAL variables enable value-assignment problems (imputation, repair, synthesis) as opposed to selection problems (BOOLEAN) or counting problems (INTEGER). They are also a prerequisite for ABS() linearization (see `sql_functions/todo.md`).

---

## Multiple Variables

Multiple decision variables may be declared in a single `DECIDE` clause, separated by commas. Each variable produces an additional output column.

```sql
DECIDE x IS BOOLEAN, y IS INTEGER
```

---

## Variable Scope

All declared variables are available in:
- `SUCH THAT` constraints
- `MAXIMIZE` / `MINIMIZE` objective
- The `SELECT` list (returned as output columns)

### Row-Scoped (Default)

By default, variables are **row-scoped**: the solver assigns one independent value per row in the input relation. When the input is a join result, each result row gets its own variable.

```sql
DECIDE x IS BOOLEAN   -- one x per result row
```

### Table-Scoped

A **table-scoped** variable is declared with a table qualifier: `DECIDE Table.var IS TYPE`. Instead of one variable per result row, the solver creates one variable per unique entity in the named source table. All result rows originating from the same entity share the same variable value — the **entity consistency guarantee**.

```sql
-- One keepN per unique nurse, not per join result row
DECIDE n.keepN IS BOOLEAN
```

**Entity identification**: All columns from the source table are used as a composite key to identify unique entities. During physical execution (Phase 1.5), the executor scans the result rows, extracts the source-table columns for each scoped variable, and builds entity-to-variable mappings. Multiple result rows with the same entity key are assigned the same solver variable index.

**Aggregate semantics**: SUM and AVG aggregate over result rows, not entities. If nurse Alice appears in 5 result rows (joined with 5 shifts), `SUM(keepN)` counts Alice's `keepN` value 5 times. This follows standard SQL aggregation semantics over the join result.

**Mixed queries**: A single DECIDE clause can declare both row-scoped and table-scoped variables:

```sql
DECIDE n.keepN IS BOOLEAN, scheduleHours IS INTEGER
```

Here `keepN` has one value per nurse entity, while `scheduleHours` has one value per result row.

**Performance**: Table-scoped variables reduce the number of solver variables from `num_rows` (join result size) to `num_entities` (distinct entities in the source table), which can significantly reduce solver time for queries with large fan-out joins.

**Limitations**:
- The table qualifier must match a table alias or name in the `FROM` clause.
- Entity keys are derived from all columns of the source table; there is no syntax to specify a custom key subset.

---

## Linearity / Non-Linearity

Linear expressions are always supported. DecidB additionally supports two classes of non-linear terms — bilinear (`x * y`) and quadratic (`x * x`, `POWER(x, 2)`) — via dedicated optimizer rewrites and solver paths:

| Expression | Status |
|---|---|
| `x * 5` — variable times constant | OK (linear) |
| `x * column` — variable times table column (constant per row) | OK (linear) |
| `x + y` — sum of variables | OK (linear) |
| `x * y` — variable times variable (bilinear) | OK — McCormick when one factor is Boolean (both solvers); non-convex QCQP otherwise (Gurobi only). See `03_expressivity/bilinear/done.md`. |
| `x * x` / `POWER(x, 2)` — variable squared (quadratic) | OK — convex QP on both solvers; non-convex QP on Gurobi only. See quadratic objectives in `syntax_reference.md`. |
| `x * y * z` — triple product and higher | **ERROR** (rejected by binder at `decide_binder.cpp`) |

---

## Use Cases by Variable Type

| Task Category | Typical Variable | Type |
|---|---|---|
| Subset selection (knapsack, sampling) | `keep` | `BOOLEAN` |
| Outlier removal | `keep` | `BOOLEAN` |
| Counterfactual explanation | `keepS`, `keepP` | `BOOLEAN` |
| Scheduling / assignment | `hours_assigned` | `INTEGER` |
| Data imputation | `imputed_distance` | `REAL` |
| Data repair | `new_hours` | `REAL` |
| Data synthesis | `syn_rent` | `REAL` |

---

## Examples

```sql
-- Basic boolean selection (row-scoped)
DECIDE keep IS BOOLEAN

-- Integer variable with default type
DECIDE quantity

-- Multiple typed variables
DECIDE x IS BOOLEAN, y IS INTEGER

-- Continuous variable for value assignment
DECIDE x IS REAL

-- Mixed types in same query
DECIDE s IS BOOLEAN, w IS REAL

-- Table-scoped: one variable per nurse entity
DECIDE n.keepN IS BOOLEAN

-- Mixed: table-scoped + row-scoped in same query
DECIDE n.keepN IS BOOLEAN, assignHours IS INTEGER
```

---

## Code Pointers

- **Grammar** (variable type rule): `third_party/libpg_query/grammar/statements/select.y`
  ```
  variable_type: INTEGER | REAL | BOOLEAN_P
  ```
- **Grammar** (comma-separated variable list, including table-qualified syntax): `select.y`
  ```
  typed_decide_variable_list: typed_decide_variable | list ',' typed_decide_variable
  ```

- **Binder** (variable processing loop, type mapping):
  `src/planner/binder/query_node/bind_select_node.cpp`
  - REAL → `LogicalType::DOUBLE`, BOOLEAN/INTEGER → `LogicalType::INTEGER`
  - Boolean type detected via `type_marker == "bool_variable"`

- **ILP model builder** (variable type handling):
  `src/decidb/utility/ilp_model_builder.cpp`
  - DOUBLE/FLOAT → `is_integer = false`, bounds `[0, 1e30]`
  - LogicalType::BOOLEAN → `is_binary = true`, bounds `[0, 1]` (only used by optimizer-created auxiliary variables: NE / IN indicators)
  - INTEGER → `is_integer = true`, bounds `[0, 1e30]`
  - Note: User-declared `IS BOOLEAN` variables are mapped to `LogicalType::INTEGER` by the binder (not `LogicalType::BOOLEAN`), with explicit `[0,1]` bounds constraints generated in `bind_select_node.cpp`. The solver result is equivalent, but the mechanism differs from optimizer-created binary auxiliaries.

- **Solver backends** (already supported continuous vars before IS REAL was enabled):
  - HiGHS: `!is_integer → HighsVarType::kContinuous` (`deterministic_naive.cpp`)
  - Gurobi: `!is_integer && !is_binary → GRB_CONTINUOUS` (`gurobi_solver.cpp`)

- **Physical execution** (DOUBLE output path): `physical_decide.cpp` — returns raw `double` solution values for REAL vars

- **Table-scoped variables**:
  - `EntityScopeInfo` struct: `src/include/duckdb/planner/operator/logical_decide.hpp` — stores the table alias and entity column indices for each scoped variable
  - `VarIndexer`: `src/include/duckdb/decidb/ilp_model.hpp` — maps entity keys to solver variable indices, deduplicating across result rows
  - Entity mapping (Phase 1.5): `src/execution/operator/decide/physical_decide.cpp` — scans result rows, extracts source-table columns, builds entity-to-variable mappings
  - Physical index resolution: `src/execution/physical_plan/plan_decide.cpp` — resolves table-scoped column references to physical indices in the execution plan
