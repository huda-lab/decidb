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

When a variable declaration uses the qualified `table.variable` syntax (e.g., `DECIDE drivers.assigned IS BOOLEAN`), the binder performs additional resolution:

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

PackDB supports both **uncorrelated and correlated scalar subqueries** in constraints.

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

Type declarations are specified in the `DECIDE` clause itself (e.g., `DECIDE x IS BOOLEAN`). The binder translates these into the appropriate internal representation and adds implicit constraints.

| DECIDE Syntax         | Internal Representation | Added Constraints     |
| :-------------------- | :---------------------- | :-------------------- |
| `DECIDE x IS INTEGER` | `x` (Type: Integer)     | `x >= 0`              |
| `DECIDE x IS BOOLEAN` | `x` (Type: Integer)     | `x >= 0` AND `x <= 1` |
| `DECIDE x`            | `x` (Type: Integer)     | `x >= 0` (default)    |
| `DECIDE x IS REAL`    | `x` (Type: Double)      | `x >= 0`              |

Note: DuckDB's internal `LogicalType::INTEGER` is used for INTEGER and BOOLEAN decision variables. `IS BOOLEAN` is strictly a domain constraint, not a storage type. `IS REAL` variables use `LogicalType::DOUBLE` internally and generate continuous (non-integer) solver variables.

## 5. `WHEN` Condition Validation

### 5.1 WHEN on Constraints

The `DecideConstraintsBinder` handles WHEN constraints via `BindWhenConstraint`:

1. **Validation**: The WHEN condition (child[1]) is checked with `ExpressionContainsDecideVariable` — it must reference only table columns, not decision variables.
2. **Constraint binding**: The constraint (child[0]) is bound through normal DECIDE constraint dispatch (linearity validation, etc.).
3. **Condition binding**: The condition (child[1]) is bound using the base `ExpressionBinder` (via `binding_when_condition` flag), bypassing DECIDE-specific validation since the condition is a data filter, not an optimization constraint.
4. **Output**: A tagged `BoundConjunctionExpression` with `alias = WHEN_CONSTRAINT_TAG` carries both the bound constraint and bound condition downstream to the execution layer.

### 5.2 WHEN on Objective

The `DecideObjectiveBinder` handles WHEN on the objective (`MAXIMIZE SUM(...) WHEN condition`) using the same pattern:

1. **Detection**: The binder checks for a `FunctionExpression` with `function_name == WHEN_CONSTRAINT_TAG` at the top level.
2. **Validation**: The WHEN condition must not reference decision variables.
3. **Objective binding**: The objective (child[0]) is bound through normal objective binding (SUM validation, linearity check).
4. **Condition binding**: The condition (child[1]) is bound via `ExpressionBinder` using the `binding_when_condition` flag bypass.
5. **Output**: A tagged `BoundConjunctionExpression` with `alias = WHEN_CONSTRAINT_TAG`, identical in structure to the constraint WHEN output.

### 5.3 Aggregate-local WHEN

Aggregate-local `WHEN` uses the same parser tag (`WHEN_CONSTRAINT_TAG`) but is bound only when it appears nested inside a larger aggregate expression, for example:

```sql
SUM(x * weight) WHEN active + SUM(x * bonus) WHEN priority <= 100
```

`DecideBinder::BindLocalWhenAggregate()` handles this form:

1. **Aggregate binding**: Child 0 is bound as a DECIDE aggregate (`SUM`, `AVG`, `MIN`, or `MAX` after normal validation).
2. **Validation**: Child 1 must not reference decision variables.
3. **Condition binding**: Child 1 is bound with the base `ExpressionBinder` and cast to BOOLEAN.
4. **Output**: The condition is stored on the resulting `BoundAggregateExpression::filter`. Downstream physical analysis copies that filter onto each extracted term from that aggregate.

The constraint and objective binders dispatch top-level `WHEN_CONSTRAINT_TAG` to expression-level WHEN binding, and nested `WHEN_CONSTRAINT_TAG` to aggregate-local binding. A global expression-level `WHEN` whose child already contains aggregate-local `WHEN` is rejected to avoid ambiguous double-filter semantics.

### 5.4 PER Constraint Validation

- PER expressions must reference a table column (not a decision variable, not a constant).
- PER is only valid on aggregate constraints (constraints using SUM/AVG).
- The PER column creates one constraint per distinct value of that column.
- Combined expression-level WHEN+PER: WHEN filters rows first, then PER groups the remaining rows.

## 6. Rewrite Passes (Now in Optimizer)

These algebraic rewrites have been migrated to `DecideOptimizer` in `src/optimizer/decide/decide_optimizer.cpp`. The binder validates and binds the relevant expressions (recognizing AVG, ABS, MIN, MAX as valid DECIDE aggregates/functions); the optimizer performs the algebraic transformations.

### 6.1 AVG → SUM Rewrite

- `AVG(expr)` is rewritten to `SUM(expr)` with a special tag (`AVG_REWRITE_TAG` alias).
- At execution time, the model builder detects this tag and scales the RHS by the number of rows in each group, effectively converting `AVG(expr) <= K` into `SUM(expr) <= K * N`.
- **Code**: `DecideOptimizer::RewriteAvgToSum` in `decide_optimizer.cpp`

### 6.2 ABS Linearization

- `ABS(expr)` where `expr` contains decision variables is linearized via auxiliary variables. Two paths depending on context:
  - Always: creates an auxiliary REAL variable `__abs_aux_N__` and emits the lower envelope `aux >= expr` and `aux >= -expr` (forces `aux >= |expr|`). Replaces `ABS(expr)` with `aux`.
  - **Path A (lower-envelope only)** — used when the surrounding context naturally pins `aux` down to `|expr|`: ABS in a `MINIMIZE` objective (solver pressure drives `aux` down) or in a constraint that upper-bounds `aux` (`ABS <= K`, `SUM(ABS) <= K`, etc.). No binary indicator allocated.
  - **Path B (Big-M sign-indicator)** — used when `aux` is not naturally pinned: `MAXIMIZE` objective ABS, and constraint shapes that don't upper-bound `aux` (`ABS >= K`, `ABS = K`, `ABS <> K`, `ABS BETWEEN`, ABS on both sides of a comparison, and the analogous aggregate forms `SUM(ABS) >= K`, `MIN(ABS) >= K`, `MAX(ABS) >= K`). Allocates a binary sign indicator `__abs_y_N__` and emits the Big-M upper envelope `aux <= expr + 2M(1-y)` and `aux <= -expr + 2M*y`, which combined with the lower envelope force `aux = |expr|` exactly.
- Path classification: `TagAbsConstraintsForBigM` walks the constraint tree before `RewriteAbs` and tags Path-B ABS function expressions with `ABS_NEEDS_BIGM_TAG` (set on `BoundFunctionExpression.alias`). `RewriteAbs` Phase 1 reads the tag, propagates `needs_bigm` to `AbsPairInfo`; Phase 2 allocates `y` for any pair where `needs_bigm || (in_objective && MAXIMIZE)`.
- The binder binds ABS as a normal `BoundFunctionExpression`. The symbolic normalization layer treats ABS as an opaque placeholder (`__ABS_N__`) to preserve it through algebraic simplification. The optimizer handles all detection, aux var creation, and constraint generation.
- **Code**: `DecideOptimizer::RewriteAbs` and `TagAbsConstraintsForBigM` in `decide_optimizer.cpp`. Tag constants `ABS_NEEDS_BIGM_TAG`, `ABS_UB_POS_TAG_PREFIX`, `ABS_UB_NEG_TAG_PREFIX` in `decide.hpp`. Full Path-A / Path-B table in `03_expressivity/sql_functions/done.md`.

> **Note**: All rewrites in this section are now implemented in `DecideOptimizer`. The binder's role is limited to semantic validation and binding.

## 7. Auxiliary Variable Management

When rewrite passes create auxiliary variables (ABS auxiliary vars, `<>` / IN indicators, MIN/MAX auxiliaries), they are tracked on the `BoundSelectNode` and carried forward to `LogicalDecide` / `PhysicalDecide`:

- **`num_auxiliary_vars`**: Count of auxiliary variables appended after user-declared variables.
- **Hiding from SELECT ***: Auxiliary variables are pruned from the bind context (lines ~887-897 of `bind_select_node.cpp`) so they don't appear in query results. They exist only in the solver's variable space.
