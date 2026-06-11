# DECIDE Clause — Implemented Features

The `DECIDE` clause declares **decision variables** of a COP query. Each variable gets a value assigned by the solver, and the assigned values appear as new columns in the query result.

**Syntax** (variable types, multiple variables, table-scoped form, examples): see `../../00_project_overview/syntax_reference.md` §1–§2. This doc covers semantics and implementation only.

---

## Semantics beyond the syntax spec

- **`IS REAL`** is stored internally as `LogicalType::DOUBLE`; both HiGHS (`kContinuous`) and Gurobi (`GRB_CONTINUOUS`) support it natively. REAL enables value-assignment problems (imputation, repair, synthesis) as opposed to selection (BOOLEAN) or counting (INTEGER), and is a prerequisite for ABS() linearization.
- **Variable scope**: declared variables are available in `SUCH THAT`, `MAXIMIZE`/`MINIMIZE`, and the `SELECT` list (returned as output columns).
- **Table-scoped entity identification**: all columns from the source table form a composite key. During physical execution (Phase 1.5), the executor scans result rows, extracts the source-table columns for each scoped variable, and maps each distinct entity key to a single solver variable index — the **entity consistency guarantee**. There is no syntax for a custom key subset.
- **Aggregate semantics with table scope**: SUM/AVG aggregate over result rows, not entities — if an entity appears in 5 join-result rows, its shared variable contributes 5 times (standard SQL aggregation over the join result).

## Linearity / Non-Linearity

Linear expressions are always supported; bilinear (`x * y`) and quadratic (`POWER(x, 2)`) terms are supported via dedicated rewrites/solver paths — see `../bilinear/done.md` and `syntax_reference.md` §4. Triple and higher products (`x * y * z`) are rejected by the binder (`decide_binder.cpp`).

## Use Cases by Variable Type

| Task Category | Typical Variable | Type |
|---|---|---|
| Subset selection (knapsack, sampling), outlier removal, counterfactuals | `keep` | `BOOLEAN` |
| Scheduling / assignment | `hours_assigned` | `INTEGER` |
| Data imputation / repair / synthesis | `imputed_distance` | `REAL` |

---

## Code Pointers

- **Grammar**: `third_party/libpg_query/grammar/statements/select.y`
  - `variable_type: INTEGER | REAL | BOOLEAN_P`
  - `typed_decide_variable_list: typed_decide_variable | list ',' typed_decide_variable` (includes table-qualified syntax)
- **Binder** (variable processing loop, type mapping): `src/planner/binder/query_node/bind_select_node.cpp`
  - REAL → `LogicalType::DOUBLE`, BOOLEAN/INTEGER → `LogicalType::INTEGER`
  - Boolean type detected via `type_marker == "bool_variable"`
- **ILP model builder** (variable type handling): `src/decidb/utility/ilp_model_builder.cpp`
  - DOUBLE/FLOAT → `is_integer = false`, bounds `[0, 1e30]`
  - `LogicalType::BOOLEAN` → `is_binary = true`, bounds `[0, 1]` (only used by optimizer-created auxiliary variables: NE / IN indicators)
  - INTEGER → `is_integer = true`, bounds `[0, 1e30]`
  - Note: user-declared `IS BOOLEAN` variables are mapped to `LogicalType::INTEGER` by the binder (not `LogicalType::BOOLEAN`), with explicit `[0,1]` bounds constraints generated in `bind_select_node.cpp`. The solver result is equivalent, but the mechanism differs from optimizer-created binary auxiliaries.
- **Solver backends**: HiGHS `!is_integer → kContinuous` (`deterministic_naive.cpp`); Gurobi `!is_integer && !is_binary → GRB_CONTINUOUS` (`gurobi_solver.cpp`)
- **Physical execution** (DOUBLE output path): `physical_decide.cpp` — returns raw `double` solution values for REAL vars
- **Table-scoped variables**:
  - `EntityScopeInfo` struct: `src/include/duckdb/planner/operator/logical_decide.hpp` — table alias + entity column indices per scoped variable
  - `VarIndexer`: `src/include/duckdb/decidb/ilp_model.hpp` — maps entity keys to solver variable indices, deduplicating across result rows
  - Entity mapping (Phase 1.5): `src/execution/operator/decide/physical_decide.cpp`
  - Physical index resolution: `src/execution/physical_plan/plan_decide.cpp`
