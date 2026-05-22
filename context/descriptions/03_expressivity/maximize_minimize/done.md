# MAXIMIZE / MINIMIZE — Implemented Features

The `MAXIMIZE` or `MINIMIZE` keyword specifies the **optimization objective** — the quantity the solver should optimize while satisfying all constraints.

---

## Syntax

```sql
MAXIMIZE objective_expression
MINIMIZE objective_expression
```

The objective clause is **optional**. Omitting it creates a feasibility problem — the solver finds any assignment satisfying all constraints without optimizing. Both Gurobi and HiGHS support feasibility problems natively.

```sql
-- Feasibility: find any valid assignment (no optimization)
SELECT * FROM shifts
DECIDE assigned IS BOOLEAN
SUCH THAT SUM(assigned) >= 3 PER day AND SUM(assigned) <= 5 PER employee
```

When present, the objective expression must be a supported aggregate expression, or an additive expression composed of supported aggregate terms.

---

## Requirements

1. Must use a supported aggregate: `SUM()`, `AVG()`, `MIN()`, or `MAX()`. See [sql_functions/done.md](../sql_functions/done.md) for details on each.
2. Must be **linear**, **quadratic** (`POWER(expr, 2)`), or **bilinear** (`x * y`) in the decision variables — see [problem_types/done.md](../problem_types/done.md) and [bilinear/done.md](../bilinear/done.md).
3. Must involve at least one decision variable.

---

## Supported Objective Forms

### SUM Over Decision Variables

```sql
MAXIMIZE SUM(x * value)            -- maximize total value of selected items
MINIMIZE SUM(x * cost)             -- minimize total cost
MAXIMIZE SUM(x)                    -- maximize number of selected items
```

### SUM with Multiple Variables

```sql
MAXIMIZE SUM(x * profit + y * bonus)
MINIMIZE SUM(x * direct_cost + y * overhead)
```

### SUM with Column Arithmetic

Decision variables may be multiplied by table columns (treated as constants per row).

```sql
MAXIMIZE SUM(x * (price - cost))   -- x * per-row constant expression
```

### Other Aggregates in Objectives

All supported aggregates work in objectives. Each is detailed in [sql_functions/done.md](../sql_functions/done.md):

- **AVG(expr)**: Flat AVG becomes SUM (same argmax/argmin). Nested with PER: inner AVG scales coefficients by `1/n_g`.
- **MIN(expr) / MAX(expr)**: Linearized via global auxiliary variable. Easy cases (`MINIMIZE MAX`, `MAXIMIZE MIN`) need only linking constraints; hard cases (`MAXIMIZE MAX`, `MINIMIZE MIN`) require Big-M indicators.
- **ABS(expr)**: Linearized via auxiliary REAL variable with two constraints (`d >= expr`, `d >= -expr`).

### Convex Quadratic Objectives (QP/MIQP)

Convex quadratic objectives are supported via `MINIMIZE SUM(POWER(linear_expr, 2))`. See [problem_types/done.md](../problem_types/done.md) for full details on syntax forms, convexity enforcement, solver support, and mathematical formulation.

```sql
MINIMIZE SUM(POWER(x - target, 2))    -- L2 minimization
```

### WHEN on Objective

The objective can be made conditional: only rows where the `WHEN` condition holds contribute. Non-matching rows have their coefficients zeroed out. See [when/done.md](../when/done.md) for full details.

```sql
MAXIMIZE SUM(x * profit) WHEN category = 'electronics'
```

Aggregate-local `WHEN` can filter individual aggregate terms in an additive objective:

```sql
MAXIMIZE SUM(x * profit) WHEN high_margin + SUM(x * bonus) WHEN strategic
```

### Composed MIN/MAX in Additive Objectives

Additive objectives may mix `MIN`/`MAX` terms with `SUM`/`AVG` terms:

```sql
MAXIMIZE MIN(x * profit) WHEN premium_tier + SUM(x * revenue)
MINIMIZE SUM(x * cost) + MAX(x * penalty) WHEN at_risk
```

Each `MIN`/`MAX` term becomes a continuous auxiliary `z_k` pinned per-row to
the inner expression; the outer sum is linear in `{x, z_k}`.

**v1 scope restrictions** (v2 will relax):
- Easy-direction terms only — `MAXIMIZE` with `MIN(...)`, or `MINIMIZE` with
  `MAX(...)`. Hard direction (`MAXIMIZE MAX`, `MINIMIZE MIN` in a composed
  sum) is rejected at bind time.
- No subtraction in the additive sum (`MAX - MIN` rejected).
- No scalar multiplication of aggregate terms (`2 * MIN(...)` rejected).
- No outer `PER`/`WHEN` wrapper on the composed objective.

See also `such_that/done.md` for composed MIN/MAX in constraints (same
mechanism).

### PER on Objective — Nested Aggregate Syntax

PER on objectives uses nested aggregate syntax: `OUTER(INNER(expr)) PER col`. See [per/done.md](../per/done.md) for full details on the two-level formulation, easy/hard classification, and WHEN+PER composition.

```sql
MINIMIZE SUM(MAX(x * cost)) PER department     -- minimize sum of per-dept max costs
MAXIMIZE MIN(SUM(x * profit)) PER region       -- maximize the worst-performing region
```

Quadratic inner expressions are supported under nested outer-`SUM`:

```sql
MINIMIZE SUM(SUM(POWER(x - target, 2))) PER grp   -- sum per-group SSE; ≡ flat SUM(POWER(...))
MINIMIZE SUM(AVG(POWER(x - target, 2))) PER grp   -- inner AVG scales each row by 1/n_g
```

These forms are detected by `SumInnerIsQuadratic` (`src/decidb/symbolic/decide_symbolic.cpp`), which preserves the raw AST through normalization so the post-bind optimizer can strip the outer wrapper. `SUM(MIN(POWER(...))) PER grp` and `SUM(MAX(POWER(...))) PER grp` also bind, but physical-layer correctness for the quadratic per-row auxiliary path is tracked separately in `context/descriptions/05_testing/quadratic/todo.md`.

---

## Objective and Solver Behavior

For linear objectives, the objective function is compiled into the `c` vector of the standard ILP formulation:

```
maximize  c^T x
subject to  Ax <= b,  x >= 0,  x integer
```

When an expression-level `WHEN` condition is present, coefficients for non-matching rows are set to 0. With aggregate-local `WHEN`, only the coefficients from that aggregate term are zeroed for non-matching rows.

For quadratic objectives, additional Q matrix terms are added. See [problem_types/done.md](../problem_types/done.md).

---

## Use Cases

| Task | Typical Objective |
|---|---|
| Knapsack / subset selection | `MAXIMIZE SUM(x * value)` |
| Minimize items selected | `MINIMIZE SUM(x)` |
| Active learning (minimize uncertainty) | `MINIMIZE SUM(keep * confidence)` |
| Maximize retained data after cleaning | `MAXIMIZE SUM(keep)` |
| Outlier removal | `MAXIMIZE SUM(keep)` |
| Entity resolution / deduplication | `MAXIMIZE SUM(keepS) + SUM(keepP)` |
| Data repair / imputation (L1) | `MINIMIZE SUM(ABS(new_val - old_val))` |
| Data repair / imputation (L2) | `MINIMIZE SUM(POWER(new_val - old_val, 2))` |

---

## Examples

```sql
MAXIMIZE SUM(x * value)
MINIMIZE SUM(x * cost)
MAXIMIZE SUM(x * value) WHEN category = 'electronics'
MAXIMIZE SUM(x * value) WHEN priority + SUM(x * bonus) WHEN strategic
MAXIMIZE SUM(keepS) + SUM(keepP)
```

---

## Code Pointers

- **Objective binder**: `src/planner/expression_binder/decide_objective_binder.cpp`
  - Validates that only `SUM`, `AVG`, `MIN`, `MAX` are used (rejects other aggregates with error message)
  - Handles WHEN condition extraction on objective
  - Dispatches nested `WHEN` on aggregate terms to aggregate-local binding
  - Binds nested aggregate PER objectives (inner/outer aggregate detection)

- **SUM argument validation**: `src/planner/expression_binder/decide_binder.cpp`
  - `ValidateSumArgumentInternal` validates the expression tree inside SUM()

- **Nested aggregate PER objective rewrite/classification**: `src/optimizer/decide/decide_optimizer.cpp`
  - `RewriteMinMaxObjective()` detects `OUTER(INNER(expr)) PER col`, sets `per_inner_*` / `per_outer_*` metadata, and rewrites inner `MIN/MAX/AVG` to `SUM`
  - Rejects flat `MIN(...) PER col` / `MAX(...) PER col` as ambiguous (requires nested aggregate form)

- **Solver input**: `src/include/duckdb/decidb/solver_input.hpp`
