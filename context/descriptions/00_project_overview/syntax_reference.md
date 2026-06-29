# Syntax Reference

## 1. The DECIDE Clause usage

```sql
SELECT ...
DECIDE [Table.]variable_name [IS type] [, [Table.]variable_name2 [IS type2] ...]
SUCH THAT
    constraint_expression
    [AND constraint_expression2 ...]
[MAXIMIZE | MINIMIZE] objective_expression
```

## 2. Decision Variables

- Must be declared in `DECIDE` list with optional type annotation.
- Scope: Available in `SUCH THAT`, `MAXIMIZE/MINIMIZE`, and the `SELECT` list.
- **Type Declarations** (in DECIDE clause):
  - `x IS INTEGER`: Default if no type specified. $x \in \{0, 1, 2, ...\}$ by default
  - `x IS BOOLEAN`: $x \in \{0, 1\}$ (automatically adds bounds constraints)
  - `x IS REAL`: $x \in [0, \infty)$ by default (continuous)
- **Default lower bound is 0** for `IS INTEGER` and `IS REAL`. This is a
  *default*, not a floor: a variable becomes **signed** (may take negative
  values) when the query gives it an explicit negative lower bound —
  `x >= -K`, `x BETWEEN -K AND K`, or a negative literal in an `IN` domain
  (`x IN (-5, 0, 5)`). A variable the query never lowers stays non-negative.
  There is no fully-free ($-\infty$) domain: signed variables always have a
  finite lower bound. See `03_expressivity/decide/done.md` → "Signed variables".

**Examples:**

```sql
DECIDE x IS BOOLEAN           -- x is binary (0 or 1)
DECIDE x IS INTEGER           -- x is integer, default domain {0, 1, 2, ...}
DECIDE x                      -- same as x IS INTEGER (default)
DECIDE x IS REAL              -- x is continuous, default domain [0, +inf)
DECIDE x IS BOOLEAN, y IS INTEGER, z IS REAL  -- multiple typed variables

-- Signed (negative-domain) variables: opt in with an explicit negative bound
DECIDE adj IS REAL            -- ... SUCH THAT adj BETWEEN -10 AND 10  → adj in [-10, 10]
DECIDE d IS INTEGER           -- ... SUCH THAT d >= -5                  → d in [-5, +inf)
```

### 2.1 Table-Scoped Variables

By default, decision variables are **row-scoped**: the solver creates one variable per row in the input relation. When a query joins multiple tables, the input relation is the join result, and each result row gets its own independent variable.

**Table-scoped** variables are declared with a table qualifier: `DECIDE Table.var IS TYPE`. A table-scoped variable has ONE value per unique entity in the named source table. All result rows originating from the same entity share the same variable value (entity consistency).

- The table qualifier must match a table alias or table name in the `FROM` clause.
- Entity identification uses all columns from the source table as a composite key.
- Mixed queries can declare both row-scoped and table-scoped variables.
- Reduces solver variable count from `num_rows` (join result size) to `num_entities` (distinct entities in the source table).

**SUM/AVG semantics**: Aggregates follow SQL semantics and sum over result rows, not entities. If an entity appears in 3 result rows (because it joined with 3 rows from another table), its variable contributes 3 times to a SUM. This matches what a user would expect from the join result.

**Example:**

```sql
-- Select nurses to keep, one decision per nurse even though each nurse
-- appears once per shift they are assigned to.
SELECT n.name, s.shift_date, keepN
FROM nurses n
JOIN shifts s ON n.id = s.nurse_id
DECIDE n.keepN IS BOOLEAN
SUCH THAT
    SUM(keepN * s.hours) <= 100
MAXIMIZE SUM(keepN * n.skill_score)
```

Here `n.keepN` is table-scoped to `nurses`: if nurse Alice appears in 5 shift rows, all 5 rows share a single `keepN` variable. Without the `n.` prefix, each of the 5 rows would get its own independent variable.

**Limitations:**
- The table qualifier must refer to a table or alias present in the `FROM` clause.
- Entity keys are derived from all columns of the source table. There is no syntax to specify a custom key subset.

## 3. Constraints

Constraints must evaluate to a boolean. Multiple constraints are separated by `AND`.

- **Supported Operators**: `=`, `<`, `<=`, `>`, `>=`, `<>`.
  - `<>` (not-equal): Supported on both per-row and aggregate constraints via Big-M disjunction (1 auxiliary binary variable + 2 constraints per `<>`). Rewritten to `LHS <= K-1 OR LHS >= K+1`, which (like strict `<` / `>`) is only valid when the LHS is integer-valued. REAL variables or non-integer coefficients are rejected with `InvalidInputException`. For `AVG(x) <> K` the denominator is hoisted to the RHS (emitted as `SUM(x) <> K*n`, per-group size for PER), keeping the LHS integer-valued.
  - `<` / `>` (strict): Rewritten internally to the integer-step form (`LHS < K` $\rightarrow$ `LHS <= ceil(K) - 1`), which is only valid when the LHS is provably integer-valued (every DECIDE variable is `IS INTEGER`/`IS BOOLEAN` and every coefficient is integral; bilinear products of integer-typed factors also count). If any term involves a `IS REAL` variable or a non-integer coefficient, DeciDB rejects the constraint with `InvalidInputException`; use `<=` / `>=` instead.
- **Between**: `expr BETWEEN a AND b` $\rightarrow$ `expr >= a AND expr <= b`.
- **In**: `x IN (v1, ..., vK)` — works on both table columns and decision variables. On decision variables, rewritten to K binary indicator variables with cardinality + linking constraints. `IN` on aggregates (e.g., `SUM(x) IN (...)`) is not supported.
- **Linearity**: Most sub-expressions involving a decision variable must be linear.
  - `x * 5`: OK.
  - `x + y`: OK.
  - `x * column`: OK (column is constant per row).
  - `x * y`: OK — bilinear (Gurobi only for non-Boolean pairs; McCormick for Boolean×anything).
  - `POWER(x - target, 2)`: OK — quadratic constraint (Gurobi only, via `GRBaddqconstr`).
  - `x * x * x`: **ERROR** (triple+ products not supported).
- **Quadratic constraints**: `POWER(linear_expr, 2)` / `expr ** 2` / `(expr)*(expr)` in constraints enables QCQP. Gurobi only. Composes with WHEN, PER. See Section 3.1 below.
- **Subqueries**: Scalar subqueries (both uncorrelated and correlated) are allowed on the RHS of constraints. Correlated subqueries are decorrelated into joins, producing per-row values. For aggregate constraints, the subquery RHS must evaluate to the same scalar for all rows. Subqueries cannot reference DECIDE variables.

### 3.1 Quadratic Constraints (QCQP)

```sql
-- Per-row quadratic constraint
SUCH THAT POWER(x - target, 2) <= 9

-- Aggregate quadratic constraint (total budget)
SUCH THAT SUM(POWER(x - target, 2)) <= 1000

-- With PER grouping
SUCH THAT SUM(POWER(x - target, 2)) <= 50 PER department

-- Multiple syntax forms (all equivalent)
SUCH THAT POWER(x - t, 2) <= K
SUCH THAT (x - t) ** 2 <= K
SUCH THAT (x - t) * (x - t) <= K
```

**Gurobi only** — HiGHS does not support quadratic constraints. Negated and scaled forms are supported: `-POWER(expr, 2)`, `K * POWER(expr, 2)`.

## 4. Objective

- **Optional**: Omitting `MAXIMIZE`/`MINIMIZE` creates a feasibility problem — the solver finds any assignment satisfying all constraints. Both Gurobi and HiGHS support this.
- When present, must be a supported aggregate expression (`SUM(...)`, `AVG(...)`, `MIN(...)`, `MAX(...)`) or an additive expression composed of supported aggregate terms.
- Must involve at least one decision variable.
- Linear objectives: must be linear in decision variables.
- **Quadratic objectives (QP)**: `MINIMIZE SUM(POWER(linear_expr, 2))` is supported for convex quadratic programming. The inner expression must be linear in decision variables. Three equivalent syntax forms:
  - `POWER(expr, 2)` / `POW(expr, 2)` — function call
  - `expr ** 2` — exponentiation operator
  - `(expr) * (expr)` — identical multiplication (both sides must be the same expression)
  - Negated forms: `-POWER(expr, 2)`, `(-1) * POWER(expr, 2)` for concave QP (both solvers).
  - `MAXIMIZE SUM(POWER(expr, 2))` is non-convex (Gurobi only, via NonConvex=2).
  - Gurobi supports both continuous QP and mixed-integer QP (MIQP). HiGHS supports continuous QP only — integer/boolean variables with quadratic objectives require Gurobi.
- **Bilinear objectives and constraints (`x * y`)**: Products of two different DECIDE variables are supported in both objectives and constraints. Two categories:
  - **Boolean x anything** (McCormick linearization): When one factor is `IS BOOLEAN`, the product is exactly linearized. Works with both Gurobi and HiGHS. Requires a finite upper bound on the non-Boolean variable — given explicitly (`x <= K`) or inferred by implied-bound propagation from a non-negative constraint like `SUM(x) <= K`. Bool x Bool uses simpler AND-linearization (no Big-M).
  - **General non-convex** (`Real*Real`, `Int*Int`, `Int*Real`): Produces indefinite Q matrix. Objectives: Gurobi only (NonConvex=2). Constraints: Gurobi only (via quadratic constraints). HiGHS rejects with clear errors.
  - Data coefficients are supported: `SUM(profit * b * x)`.
  - Composes with WHEN: `SUM(b * x) WHEN condition`.
  - Triple or higher products (`a * b * c`) are rejected.

## 5. Aggregations

- `SUM()` is the primary aggregate over decision variables.
- `AVG(expr)` is supported. Rewritten to `SUM(expr)` with RHS scaled by row count N at execution time. For objectives, `AVG` and `SUM` share the same argmax/argmin. For constraints, `AVG(expr) op K` becomes `SUM(expr) op K*N` where N is the row count (adjusted for WHEN/PER context).
- `SUM`/`AVG` constraint bodies may include data-only additive terms alongside decision terms, e.g. `SUM(x + cost) <= K` or `SUM(q * (price + x)) <= K`. DeciDB evaluates the data-only aggregate contribution over the active rows/group and subtracts it from the scalar RHS before building the solver row.
- `MIN(expr)` and `MAX(expr)` are supported. Easy cases (`MAX(expr) <= K`, `MIN(expr) >= K`) become per-row constraints with no auxiliary variables. Hard cases (opposite direction, equality) use a global auxiliary variable and Big-M binary indicators. In objectives, `MINIMIZE MAX(expr)` and `MAXIMIZE MIN(expr)` use a global auxiliary; `MAXIMIZE MAX(expr)` and `MINIMIZE MIN(expr)` additionally require Big-M indicators. Composes with WHEN.
- Aggregate-local filters are supported on individual aggregate terms: `SUM(expr) WHEN condition + SUM(expr2) WHEN condition2`. This is different from an expression-level `WHEN` on the whole constraint or objective.
- `norm(expr, p)` expresses an L_p regularization term over a decision-variable expression (lasso/ridge-style). The user supplies the weight, e.g. `MINIMIZE SUM(cost*x) + 0.5 * norm(x - base, 1)`. It is desugared at bind time into existing supported forms, so it composes with WHEN/PER and works in both objectives and constraints (including norm-bounded constraints like `norm(e, 1) <= K`). Supported orders:
  - `norm(e, 1)` → `SUM(ABS(e))` — L1 (sparse-leaning; ABS linearization).
  - `norm(e, 2)` → `SUM(POWER(e, 2))` — squared L2 / ridge (convex QP).
  - `norm(e, 'inf')` → `MAX(ABS(e))` — L-infinity (max linearization).
  - `norm(e, 0[, M])` → **L0 / count of nonzeros**. Adds one 0/1 indicator `z` per row linked so `z = 1` whenever `e != 0`; the term becomes `SUM(z)`. Upgrades the model to a MILP. The Big-M bound is **inferred from the data** when omitted — `norm(e, 0)` derives a tight per-problem `M` at execution from variable bounds (implied-bound propagation) + data; pass `norm(e, 0, M)` to supply it explicitly (e.g. when bounds aren't inferable, where the auto path would otherwise fall back to a loose default). Useful as an objective penalty or as an exact-count cap: `norm(e, 0[, M]) <= K`.

## 6. Conditional Expressions — `WHEN`

The `WHEN` keyword enables conditional constraints and conditional objectives. A `WHEN` clause causes the expression to apply only to rows where the condition evaluates to true. Rows where the condition is false or NULL are excluded.

**Syntax**: `expression WHEN condition`

### 6.1 WHEN on Constraints

```sql
SUCH THAT
    SUM(x * weight) <= 50 WHEN category = 'electronics' AND
    SUM(x * weight) <= 30 WHEN category = 'clothing' AND
    x <= 1
```

**Execution**: WHEN conditions create per-row boolean masks. For aggregate constraints (`SUM`), masked-out rows are excluded from the summation. For per-row constraints, masked-out rows skip constraint generation entirely.

### 6.2 WHEN on Objective

```sql
MAXIMIZE SUM(x * profit) WHEN category = 'electronics'
MINIMIZE SUM(x * cost) WHEN region = 'US'
```

**Execution**: Objective coefficients for non-matching rows are zeroed out. The solver sees a standard ILP where only matching rows contribute to the objective value.

### 6.3 Aggregate-local WHEN

`WHEN` can also be attached directly to a single aggregate term inside an additive aggregate expression:

```sql
SUCH THAT
    SUM(x * hours) WHEN morning + SUM(x * hours) WHEN evening <= 40

MAXIMIZE
    SUM(x * profit) WHEN high_margin + SUM(x * bonus) WHEN strategic
```

Each aggregate-local `WHEN` filters only that aggregate's rows. Rows outside all local aggregate filters do not contribute to that expression, but they are not removed from the query or from unrelated constraints/objectives.

Comparison predicates in aggregate-local `WHEN` conditions must be parenthesized:

```sql
SUM(x * hours) WHEN (shift = 'morning') + SUM(x * hours) WHEN (shift = 'evening') <= 40
```

Do not combine expression-level `WHEN` with aggregate-local `WHEN` in the same constraint or objective; the binder rejects that shape to avoid ambiguous double-filter semantics.

### 6.4 Rules (applies to both constraints and objectives)

- `WHEN` is postfix: the expression comes first, then `WHEN condition`.
- The `WHEN` condition must reference only table columns, **not** decision variables.
- NULL conditions are treated as false (expression does not apply for that row).
- When a `WHEN` condition contains `AND`/`OR`, parentheses are required:
  ```sql
  SUM(x * weight) <= 20 WHEN (category = 'A' AND status = 'active')
  ```
- Expressions without `WHEN` apply unconditionally to all rows.

## 7. Group-Scoped Constraints — `PER`

The `PER` keyword generates one constraint per distinct value (or combination of values) of column(s).

**Syntax**: `SUM(expr) comparison rhs PER column` or `PER (col1, col2, ...)`

```sql
SUCH THAT
    SUM(x * hours) <= 40 PER empID
    SUM(x * hours) <= 40 PER (empID, department)
```

Column references in `PER` can be either bare (`PER empID`) or qualified by table name / alias (`PER employees.empID`, `PER e.empID`). Qualified and unqualified forms are equivalent; the qualifier is purely syntactic disambiguation, useful for JOIN queries where the grouping column would otherwise be ambiguous. Mixed forms (`PER (empID, dept.id)`) are accepted.

### 7.1 PER + WHEN Composition

WHEN filters rows first, then PER groups the remaining rows:

```sql
SUCH THAT
    SUM(x * hours) <= 30 WHEN title = 'Director' PER empID
    SUM(x * hours) <= 30 WHEN title = 'Director' PER (empID, department)
```

### 7.2 PER on Objective — Nested Aggregates

PER on objectives uses nested aggregate syntax to specify both per-group and across-group aggregation:

```sql
-- Nested aggregate: OUTER(INNER(expr)) PER col
MINIMIZE SUM(MAX(x * cost)) PER department
MAXIMIZE MIN(SUM(x * profit)) PER region
MINIMIZE MAX(SUM(x * hours)) PER empID
```

All 9 combinations of `SUM`/`MIN`/`MAX` for outer and inner aggregates are supported.

**Flat aggregate + PER**:
- `SUM(expr) PER col` or `AVG(expr) PER col`: Accepted (no-op — global sum equals sum of group sums).
- `MIN(expr) PER col` or `MAX(expr) PER col` (flat): **Error** — use nested form instead.

WHEN + PER composition is supported: `MINIMIZE MAX(SUM(x * hours)) WHEN active = 1 PER empID`.

### 7.3 Restrictions

- **Aggregate-only**: PER requires an aggregate constraint, such as `SUM(...)`, `AVG(...)`, or an additive expression of aggregate terms. Per-row constraints are rejected.
- **Column references only**: Each PER column must be a simple column reference, not an expression or decision variable.
- **Constant RHS**: The right-hand side must be constant across groups.
- **NULL handling**: Rows where any PER column is NULL are excluded.
