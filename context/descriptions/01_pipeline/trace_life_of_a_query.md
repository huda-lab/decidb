# Life of a Query: The "Knapsack" Trace

This document provides a low-level execution trace of a characteristic decision query. It shows exactly how data transforms from SQL source to Final Result.

## 1. The Input Query

We use a standard "Knapsack" problem: Select items to maximize value while keeping weight under limits.

```sql
SELECT id, value, weight
FROM Items
WHERE category = 'electronics'
DECIDE x(BOOL)
SUCH THAT
    SUM(x * weight) <= 50
MAXIMIZE SUM(x * value);
```

**Table Data (`Items`)**:
| id | value | weight | category |
|---:|---:|---:|---|
| 1 | 100 | 20 | electronics |
| 2 | 60 | 10 | electronics |
| 3 | 120 | 30 | electronics |
| 4 | 50 | 50 | furniture |

## 2. Parser & Symbolic Phase

**Input**: Raw SQL String.
**Action**: The parser identifies the `DECIDE` block.

1.  **Normalization**:
    - Constraint `SUM(x * weight) <= 50` is identified.
    - LHS `SUM(x * weight)` contains `x` (decision var).
    - RHS `50` is constant.
    - Constraint is valid.

## 3. Binder Phase

**Input**: Parsed Statement.
**Action**: Validates types and names.

1.  `DECIDE x(BOOL)`: The binder extracts the type from the DECIDE clause and records that variable `x` has specialized domain $[0, 1]$. It automatically adds implicit constraints `x >= 0 AND x <= 1`.
2.  **Filter**: `WHERE category='electronics'` is bound to a standard Table Scan + Filter.
3.  **Linearity Check**: `x * weight` is valid because `weight` comes from the table (constant coefficient).

## 4. Logical Planner

**Output**: A Logical Operator Tree.

```
LogicalDecide
 ├── Variables: [x]
 ├── Constraints: [SUM(x*weight) <= 50]
 ├── Objective: MAX SUM(x*value)
 └── Children:
      LogicalFilter (category = 'electronics')
       └── LogicalGet (Items)
```

## 5. Physical Execution Trace

### Step A: Materialization (Sink)

The `PhysicalDecide` operator pulls from the child.

- Row (id=4) is filtered out by `LogicalFilter`.
- Rows 1, 2, 3 enter the Sink.

**Buffered State (`DecideGlobalSinkState`)**:
| RowIdx | id | value | weight |
|---:|---:|---:|---:|
| 0 | 1 | 100 | 20 |
| 1 | 2 | 60 | 10 |
| 2 | 3 | 120 | 30 |

### Step B: Coefficient Evaluation

The operator evaluates the constraint expressions for each buffered row.

**Constraint 1**: `SUM(x * weight) <= 50`

- Row 0: `weight` evaluates to 20. Term: $20 \cdot x_0$.
- Row 1: `weight` evaluates to 10. Term: $10 \cdot x_1$.
- Row 2: `weight` evaluates to 30. Term: $30 \cdot x_2$.

**Objective**: `MAX SUM(x * value)`

- Coeffs: $100 \cdot x_0, 60 \cdot x_1, 120 \cdot x_2$.

### Step C: Solver Model (SolverModel)

The operator builds a `SolverInput`, which `SolverModel::Build()` transforms into the following ILP model (conceptually):

```
Maximize
 obj: 100 x0 + 60 x1 + 120 x2
Subject To
 c1: 20 x0 + 10 x1 + 30 x2 <= 50
Bounds
 0 <= x0 <= 1
 0 <= x1 <= 1
 0 <= x2 <= 1
Integers
 x0 x1 x2
End
```

### Step D: Solving

The solver facade (`SolveModel()`) dispatches to Gurobi (preferred) or HiGHS (fallback) to solve the model.

- **Solution**: $x_0 = 1, x_1 = 0, x_2 = 1$.
- **Check**: Weight $20(1) + 10(0) + 30(1) = 50 \leq 50$. (Valid)
- **Value**: $100 + 0 + 120 = 220$. (Optimal)

### Step E: Result Projection (Source)

The operator re-scans the buffer and appends the solution.

**Output Chunk**:
| id | value | weight | x (DECIDE) |
|---:|---:|---:|---:|
| 1 | 100 | 20 | 1 |
| 2 | 60 | 10 | 0 |
| 3 | 120 | 30 | 1 |
