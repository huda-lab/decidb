# Implementation Part 2: Binder & Validation

## 1. Overview

After parsing, the `DECIDE` clause needs semantic validation. The Binder is responsible for ensuring that the user's query makes sense in the context of the database schema and the mathematical constraints of the solver.

**Key Source Files**:

- `src/planner/expression_binder/decide_binder.cpp`
- `src/planner/expression_binder/decide_constraints_binder.cpp`

## 2. Variable Scope & Binding

Unlike a standard `GROUP BY` or `SELECT` clause, the `DECIDE` clause introduces variables that do not exist in any physical table.

- **Decide Variables**: These are "virtual" columns representing the decision.
- **Binding Context**: The binder creates a special scope where these variables are valid. It verifies that variable names do not collide with existing table columns.

### 2.1 Table-Scoped (Entity-Scoped) Variables

When a variable declaration uses the qualified `table.variable` syntax (e.g., `DECIDE drivers.assigned(BOOL)`), the binder performs additional resolution:

1. **Table alias resolution**: The binder looks up the table alias (e.g., `drivers`) in the current bind context to verify that the referenced table exists in the FROM clause.
2. **Entity key identification**: The binder identifies the entity key columns for the referenced table. These are the columns that define unique entities (typically a primary key or the columns used to distinguish rows belonging to the same entity).
3. **EntityScopeInfo creation**: The binder creates an `EntityScopeInfo` struct containing the table alias, the source table index, column bindings for the entity keys, and the indices of scoped variables. This struct is stored on the `BoundSelectNode` and carried forward to `LogicalDecide`.
4. **Validation**: The binder rejects invalid table aliases (table not found in FROM clause) and ensures entity key columns can be resolved.

The `EntityScopeInfo` struct (defined in `logical_decide.hpp`) contains:
- `table_alias`: The alias used in the DECIDE clause
- `source_table_index`: Index of the source table in the plan
- `entity_key_bindings`: Column bindings for the entity key columns
- `entity_key_physical_indices`: Physical chunk positions (populated later during plan creation)
- `scoped_variable_indices`: Which DECIDE variables are scoped to this entity

## 3. Constraint Validation

The primary role of the `DecideConstraintsBinder` is to enforce the **Linearity Assumption** required by ILP solvers.

### 3.1 Linearity Check

Every term on the LHS of a constraint must optionally involve a Decide Variable, but never in a non-linear way.

- **Valid**: `2*x`, `x`, `price * x` (assuming `price` is a table column, constant for the decision).
- **Invalid**: `x * x` (Quadratic), `x * y` (Interaction), `SIN(x)`.

The binder walks the expression tree and flags an error if it encounters a multiplication between two sub-trees that both contain decision variables.

### 3.2 Subquery Handling

DeciDB supports both **uncorrelated and correlated scalar subqueries** in constraints.

- Example (uncorrelated): `SUM(x) <= (SELECT COUNT(*) FROM Drivers)`
- Example (correlated): `x <= (SELECT budget FROM Depts WHERE Depts.id = items.dept_id)`
- **Mechanism**: Subqueries are delegated to DuckDB's standard `ExpressionBinder::BindExpression`, which handles both cases via `PlanSubqueries`:
  - **Uncorrelated**: Evaluated once as cross-joined scalars (constant RHS).
  - **Correlated**: Decorrelated into joins, producing per-row values.
- **Validation**:
  - Only scalar subqueries are supported (non-scalar returns an error).
  - Subqueries cannot reference DECIDE variables (checked via `ExpressionContainsDecideVariable`, defined in `decide_binder.cpp` and invoked at multiple subquery/WHEN call sites).
  - For aggregate constraints (`SUM`, `AVG`), the RHS must be a scalar (same value for all rows). This is validated at execution time in `ilp_model_builder.cpp` — if the correlated subquery produces different values per row, an error is thrown.

### 3.3 Operator Restrictions

- **IN operator**: `IN` on decision variables is supported via rewrite to K auxiliary binary indicator variables (one per value in the set), with cardinality and linking constraints. See Section 6.1.
- **Standard comparisons** (`=`, `<`, `<=`, `>`, `>=`, `<>`, `BETWEEN`): Supported on both per-row and aggregate constraints.

## 4. Type Inference & Syntactic Sugar

Type declarations are specified in the `DECIDE` clause itself (e.g., `DECIDE x(BOOL)`). The binder translates these into the appropriate internal representation and adds implicit constraints.

| DECIDE Syntax         | Internal Representation | Added Constraints     |
| :-------------------- | :---------------------- | :-------------------- |
| `DECIDE x(INT)` | `x` (Type: Integer)     | `x >= 0`              |
| `DECIDE x(BOOL)` | `x` (Type: Integer)     | `x >= 0` AND `x <= 1` |
| `DECIDE x(REAL)`    | `x` (Type: Double)      | `x >= 0`              |

Note: DuckDB's internal `LogicalType::INTEGER` is used for INT and BOOL decision variables. `BOOL` is strictly a domain constraint, not a storage type. `REAL` variables use `LogicalType::DOUBLE` internally and generate continuous (non-integer) solver variables.

## 5. `WHEN` Condition Validation

WHEN semantics, NULL handling, and restrictions live in `../03_expressivity/when/done.md`; the binder-side mechanics are:

- **Expression-level WHEN** (constraints via `DecideConstraintsBinder::BindWhenConstraint`, objectives via `DecideObjectiveBinder`): validate the condition with `ExpressionContainsDecideVariable` (table columns only), bind the constraint/objective through normal DECIDE dispatch, bind the condition with the base `ExpressionBinder` (`binding_when_condition` flag bypasses DECIDE validation — it's a data filter, not a constraint). Output: a tagged `BoundConjunctionExpression` with `alias = WHEN_CONSTRAINT_TAG` carrying both children downstream.
- **Aggregate-local WHEN** (same parser tag, but nested inside a larger aggregate expression): `DecideBinder::BindLocalWhenAggregate()` binds child 0 as a DECIDE aggregate, validates child 1 (no decide vars), binds it as BOOLEAN, and stores it on `BoundAggregateExpression::filter`. Downstream physical analysis copies that filter onto each extracted term.
- **Dispatch**: top-level `WHEN_CONSTRAINT_TAG` → expression-level binding; nested → aggregate-local. A global WHEN whose child already contains aggregate-local WHEN is rejected (ambiguous double-filter semantics).

### 5.4 PER Constraint Validation

- PER expressions must reference a table column (not a decision variable, not a constant).
- PER is only valid on aggregate constraints (constraints using SUM/AVG).
- The PER column creates one constraint per distinct value of that column.
- Combined expression-level WHEN+PER: WHEN filters rows first, then PER groups the remaining rows.

## 6. Rewrite Passes (Now in Optimizer)

The algebraic rewrites (AVG→SUM with `AVG_REWRITE_TAG`, ABS linearization with Path-A/Path-B classification, MIN/MAX classification, `<>` indicators, bilinear McCormick) live in `DecideOptimizer` (`src/optimizer/decide/decide_optimizer.cpp`) — see `../04_optimizer/rewrite_passes/done.md` for the inventory and `../03_expressivity/sql_functions/done.md` for per-function details. The binder's role is limited to semantic validation and binding: it recognizes AVG, ABS, MIN, MAX as valid DECIDE aggregates/functions and binds them as normal bound expressions (ABS passes through symbolic normalization as an opaque `__ABS_N__` placeholder); the optimizer performs all detection, auxiliary-variable creation, and constraint generation.

## 7. Auxiliary Variable Management

When rewrite passes create auxiliary variables (ABS auxiliary vars, `<>` / IN indicators, MIN/MAX auxiliaries), they are tracked on the `BoundSelectNode` and carried forward to `LogicalDecide` / `PhysicalDecide`:

- **`num_auxiliary_vars`**: Count of auxiliary variables appended after user-declared variables.
- **Hiding from SELECT ***: Auxiliary variables are pruned from the bind context (lines ~887-897 of `bind_select_node.cpp`) so they don't appear in query results. They exist only in the solver's variable space.
