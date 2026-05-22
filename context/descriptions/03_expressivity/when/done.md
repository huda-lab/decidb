# WHEN Keyword — Implemented Features

`WHEN` is a postfix conditional modifier applied to both constraints (in `SUCH THAT`) and objectives (`MAXIMIZE`/`MINIMIZE`). It causes the expression to apply only to rows where the condition is true.

---

## Syntax

```sql
-- On a constraint
constraint_expression WHEN condition

-- On an objective
MAXIMIZE SUM(...) WHEN condition
MINIMIZE SUM(...) WHEN condition

-- On one aggregate term inside an additive aggregate expression
SUM(expr1) WHEN condition1 + SUM(expr2) WHEN condition2 <= rhs
MAXIMIZE SUM(expr1) WHEN condition1 + SUM(expr2) WHEN condition2
```

`WHEN` always appears *after* the main expression and *before* the `AND` separator.

---

## Semantics

### WHEN on Aggregate Constraints

For aggregate constraints (those using `SUM`), `WHEN` filters which rows are included in the aggregate. Rows where the condition is false (or NULL) are excluded. The solver sees a standard linear inequality with coefficients zeroed out for non-matching rows.

```sql
SUM(x * weight) <= 50 WHEN category = 'electronics'
```

Equivalent formulation (for illustration):
```sql
SUM(CASE WHEN category = 'electronics' THEN x * weight ELSE 0 END) <= 50
```

### WHEN on Per-Row Constraints

For constraints without an aggregate, `WHEN` causes the constraint to be *skipped entirely* for non-matching rows. No constraint is generated for those rows in the ILP.

```sql
x <= 1 WHEN status = 'active'    -- no constraint generated for inactive rows
```

### WHEN on Objective

For the objective, `WHEN` zeros out the contribution of non-matching rows. The solver sees a standard dense objective vector with zeros for excluded rows.

```sql
MAXIMIZE SUM(x * profit) WHEN category = 'electronics'
```

### WHEN with Not-Equal (`<>`) Constraints

Both expression-level and aggregate-local `WHEN` compose correctly with `<>` (not-equal) aggregate constraints:

```sql
-- Expression-level WHEN + NE
SUM(x) <> 2 WHEN active

-- Aggregate-local WHEN + NE
SUM(x) WHEN active <> 2
```

The NE Big-M disjunction uses a **single global binary indicator variable** per group (one for WHEN-only, one per group for PER). This is expanded as raw constraints after the `VarIndexer` is built, bypassing the per-row indicator path used by per-row NE constraints. The formulation is:
- `SUM(coeffs) - M*z <= K-1`  (z=0 → SUM ≤ K-1)
- `SUM(coeffs) - M*z >= K+1-M`  (z=1 → SUM ≥ K+1)

Code pointer: deferred aggregate NE expansion in `physical_decide.cpp`, after the single `VarIndexer var_indexer` is built (the same indexer is later threaded through `SolveModel()` and ultimately moved onto `gstate.var_indexer`).

### Aggregate-local WHEN

Aggregate-local `WHEN` attaches to a single aggregate, not to the whole constraint or objective. This lets one expression use different row filters for different aggregate terms:

```sql
SUM(x * weight) WHEN category = 'electronics'
  + SUM(x * weight) WHEN category = 'clothing' <= 80

MAXIMIZE
  SUM(x * profit) WHEN high_margin
  + SUM(x * strategic_bonus) WHEN strategic_account
```

The two filters are independent. A row matching only `category = 'electronics'` contributes only to the first aggregate term; a row matching only `category = 'clothing'` contributes only to the second.

This is intentionally separate from expression-level `WHEN`:

```sql
-- One global filter on the whole constraint
SUM(x * weight) + SUM(x * extra_weight) <= 80 WHEN active

-- Independent filters per aggregate term
SUM(x * weight) WHEN active + SUM(x * extra_weight) WHEN expedited <= 80
```

Comparison predicates in aggregate-local conditions must be parenthesized:

```sql
SUM(x * weight) WHEN (category = 'electronics')
  + SUM(x * weight) WHEN (category = 'clothing') <= 80
```

Without parentheses, existing expression-level objective syntax such as `MAXIMIZE SUM(x * profit) WHEN category = 'electronics'` is preserved as a whole-objective `WHEN`.

---

## NULL Handling

If the `WHEN` condition evaluates to NULL for a row, that row is treated as **not matching** (same as false).

---

## Empty Row Sets

A `WHEN` filter that matches zero rows on an aggregate (SUM, AVG, MIN, MAX) is **rejected pre-solver** with `InvalidInputException`:

```
DECIDE empty row set for {aggregate|min|max|sum|avg} in {constraint|objective|composed constraint|composed objective}.
An empty aggregate has no well-defined value; check your WHEN clause.
```

**Scope** — rejected cases:
- Constraint- or objective-level `WHEN` that filters every row: `SUM(x*v) <= K WHEN false_condition`.
- Aggregate-local `WHEN` on any single term (SUM/AVG/MIN/MAX) that matches zero rows.
- Composed MIN/MAX term with empty `WHEN`: `SUM(x*v) + (MAX(x*v) WHEN false) <= K`.
- Easy-direction MIN/MAX (`MAX(...) <= K WHEN …`, `MIN(...) >= K WHEN …`): the optimizer strips these to per-row, but a `MINMAX_EASY_REWRITE_TAG` on the rewritten comparison lets the execution layer still enforce the rule.

**Not rejected** (preserved behavior):
- Individual empty groups within `PER`: those groups are skipped downstream. Only rejected when **every** group is empty (the aggregate as a whole sees no rows).
- Per-row constraints with `WHEN` that matches zero rows (e.g., `x <= 0 WHEN never_condition`): this is a valid no-op (the constraint applies to no rows).

**Rationale**: The MIN/MAX reformulation uses a global auxiliary `z` (or per-term `z_k` in composed shapes) that the solver pins via per-row constraints. With zero rows the auxiliary has no per-row pinning and floats free inside its bounds — the outer constraint or objective becomes silently vacuous. Semantically MIN(∅)=+∞ and MAX(∅)=−∞, so a hard-direction bound should be infeasible rather than ignored. Rejecting pre-solver is cleaner than emitting `0 >= 1` and reporting solver infeasibility — the error points directly at the likely `WHEN` typo. SUM and AVG are rejected for consistency (strict "reject all empty sets" rule) even though SUM(∅)=0 and AVG(∅) would only be mathematically undefined in the pure-aggregate case.

**Code pointers**:
- Guard helper: `RejectEmptyAggregate` in `src/execution/operator/decide/physical_decide.cpp`.
- Four insertion sites: constraint `row_group_ids` build (after the WHEN/PER unified evaluation), objective WHEN/per-term filter application, composed MIN/MAX constraint `z_k` loop, composed MIN/MAX objective `z_k` loop.
- Easy-direction tag: `MINMAX_EASY_REWRITE_TAG` in `src/include/duckdb/common/enums/decide.hpp`, set during `RewriteMinMaxInConstraint` in `src/optimizer/decide/decide_optimizer.cpp`, detected at physical plan construction to set `DecideConstraint.was_minmax_easy`.

---

## Rules and Restrictions

### Conditions Must Reference Only Table Columns

`WHEN` conditions may only reference columns from the input relation. Decision variables are **not allowed** in `WHEN` conditions.

```sql
-- OK
SUM(x * weight) <= 50 WHEN category = 'electronics'

-- ERROR: decision variable in WHEN condition
SUM(x * weight) <= 50 WHEN x = 1
```

**Reason**: `WHEN` conditions are evaluated before the solver runs, to construct the coefficient matrix.

### Compound Conditions Require Parentheses

When a `WHEN` condition uses `AND` or `OR`, it must be wrapped in parentheses to avoid ambiguity with the `AND` constraint separator.

```sql
-- Correct
SUM(x * weight) <= 20 WHEN (category = 'A' AND status = 'active')

-- Incorrect: the parser treats AND as constraint separator
SUM(x * weight) <= 20 WHEN category = 'A' AND status = 'active'
```

### Unparenthesized `NOT`, Comparisons, and Arithmetic Are Rejected

The grammar production for `WHEN` conditions (`decide_when_condition = c_expr`) excludes `NOT`, comparison operators, and arithmetic. Wrapping the condition in parentheses forces it through a different grammar path that supports the full operator set:

```sql
-- Rejected by the parser
SUM(x * v) <= 12 WHEN NOT w
SUM(x * v) WHEN tier = 'high' <= 10      -- also misparses in constraints

-- Correct: parenthesize the condition
SUM(x * v) <= 12 WHEN (NOT w)
SUM(x * v) WHEN (tier = 'high') <= 10
```

See `../when/todo.md` for the grammar-asymmetry limitation (constraint-side error messages for this case are misleading).

### Expressions Without WHEN Apply Unconditionally

A constraint or objective without `WHEN` applies to all rows.

### Expression-level and Aggregate-local WHEN Do Not Mix

DecidB rejects a constraint or objective that contains both a whole-expression `WHEN` and one or more aggregate-local `WHEN` filters:

```sql
-- ERROR: expression-level WHEN and aggregate-local WHEN in same constraint
(SUM(x * weight) WHEN active + SUM(x * weight) WHEN expedited <= 80) WHEN region = 'US'
```

Move the shared condition into each aggregate-local filter, or keep one expression-level `WHEN`.

### Aggregate-local WHEN Composes with Constraint-LHS Arithmetic

Constant offsets and constant scalar factors on a WHEN-tagged aggregate are supported in constraints — the symbolic normalizer rewrites them at the parsed-expression level (without touching the WHEN tag) before the per-row extractor runs:

```sql
-- All OK: WHEN-tagged aggregate composed with constants on the constraint LHS
SUCH THAT SUM(x) WHEN (w > 1) + 3 <= 10                  -- offset peeled to RHS
SUCH THAT 2 * (SUM(x) WHEN active) <= 10                 -- scalar folded into SUM body
SUCH THAT (SUM(x) WHEN active) / 2 <= 5                  -- divisor folded into SUM body
SUCH THAT (SUM(x) WHEN w) + (SUM(y) + 3) <= 10           -- nested parallel sum + offset
```

The outer-WHEN form remains supported and equivalent for single-aggregate constraints:

```sql
SUCH THAT SUM(x) + 3 <= 10 WHEN active
```

**How it works**: `NormalizeComparisonExpr` in `src/decidb/symbolic/decide_symbolic.cpp` can't run the SymEngine expand/simplify path on a WHEN-bearing LHS — that would flatten the per-aggregate filter. Instead it does a parsed-level rewrite: (1) folds `K * (SUM(...) WHEN c)` (and `(SUM(...) WHEN c) / K`) into `WHEN(SUM(K * inner), c)` so the downstream extractor sees a bare WHEN-tagged aggregate; (2) decomposes the LHS additively (recursing through `+`, binary `-`, unary `-`, CAST), peels pure-numeric leaves into a single offset; (3) rebuilds the LHS from the structural terms and emits `LHS_struct OP (RHS - offset)`.

Objectives get the same treatment as constraints: `MAXIMIZE (SUM(x) WHEN cond) + 3`, `MAXIMIZE 2 * (SUM(x) WHEN cond)`, and combinations like `MINIMIZE SUM(x) + SUM(y) WHEN c - 7` are all supported. Additive constants don't affect `argmax`/`argmin` so the peel drops them from the body (stored on `LogicalDecide.objective_constant_offset` for any future feature that reports the objective value); `K * AGG` and `AGG / K` fold the scalar into the aggregate body via the same rewrite used for constraints.

> **Parser limitation (unchanged)**: writing `SUM(x) WHEN cond + 3 <= K` without parentheses around the condition is a plain `Parser Error` because aggregate-local `WHEN` binds tighter than `>`/`<=` per `POSTFIXOP` precedence, and `%nonassoc` comparisons can't chain. Use `SUM(x) WHEN (cond) + 3 <= K` (parens around the condition).

---

## Examples

```sql
-- Constraint: different weight limits per category
SUCH THAT
    SUM(x * weight) <= 50 WHEN category = 'electronics' AND
    SUM(x * weight) <= 30 WHEN category = 'clothing' AND
    x <= 1

-- Constraint: compound condition (parentheses required)
SUCH THAT
    SUM(x * weight) <= 20 WHEN (category = 'A' AND status = 'active')

-- Objective: only count electronics toward the objective
MAXIMIZE SUM(x * profit) WHEN category = 'electronics'

-- Objective: independent aggregate-local filters
MAXIMIZE
    SUM(x * profit) WHEN high_margin +
    SUM(x * bonus) WHEN strategic_account

-- Constraint + objective both using WHEN
SUCH THAT
    SUM(x * weight) <= 100 WHEN region = 'US'
MAXIMIZE SUM(x * value) WHEN region = 'US'
```

---

## Note: WHEN vs SQL CASE WHEN

DecidB's `WHEN` is a **row filter** — it controls whether a constraint or objective *applies* to a row. SQL's `CASE WHEN` is a **value expression** — it produces different values conditionally. These serve different purposes and are not interchangeable.

When you need **conditional coefficients or bounds** (different values per row based on conditions), use a CTE or subquery to pre-compute the value, then reference the resulting column inside DECIDE. This avoids any need to support `CASE WHEN` within the DECIDE clause itself.

### Example 1: Conditional Penalty Weights in Objective

Suppose director hour changes should be penalized 3x and manager changes 2x:

```sql
WITH weighted AS (
  SELECT *,
    CASE WHEN title = 'Director' THEN 3
         WHEN title = 'Manager'  THEN 2
         ELSE 1 END AS penalty_weight
  FROM Employees E JOIN WeeklyPlan P ON E.empID = P.empID
)
SELECT *
FROM weighted
DECIDE new_hours IS INTEGER
SUCH THAT ...
MINIMIZE SUM(penalty_weight * abs(new_hours - hours))
```

`penalty_weight` is a table column by the time DECIDE sees it, so it works as a standard coefficient.

### Example 2: Conditional Effectiveness in Constraint

Suppose director hours count as twice as effective:

```sql
WITH effective AS (
  SELECT *,
    CASE WHEN title = 'Director' THEN 2 ELSE 1 END AS effectiveness
  FROM Employees E JOIN WeeklyPlan P ON E.empID = P.empID
)
SELECT *
FROM effective
DECIDE new_hours IS INTEGER
SUCH THAT
  SUM(new_hours * effectiveness) >= 60 PER projectID
```

Note: this is **not equivalent** to decomposing into separate WHEN constraints (`SUM(new_hours) >= 60 PER projectID AND SUM(new_hours) >= 30 WHEN title='Director' PER projectID`), which creates two independent constraints rather than one combined weighted sum.

### Example 3: Conditional Bounds with Overlapping Conditions

Suppose rent tolerance depends on zipcode and bedroom count, with overlapping conditions:

```sql
WITH toleranced AS (
  SELECT *,
    CASE WHEN zipcode = 10003 THEN 500
         WHEN beds >= 3       THEN 300
         ELSE 150 END AS tolerance
  FROM rentals
)
SELECT *
FROM toleranced
DECIDE syn_rent IS INTEGER
SUCH THAT
  abs(syn_rent - rent) <= tolerance
```

Decomposing into multiple WHEN constraints fails here because the conditions overlap (e.g., a 3-bed apartment in zipcode 10003). SQL `CASE WHEN` evaluates top-to-bottom and returns the first match, giving the correct priority semantics. Pre-computing it as a column preserves that behavior.

### Summary

| Need | Use |
|------|-----|
| Include/exclude rows from a constraint or objective | `WHEN` (postfix) |
| Different coefficient values per row | `CASE WHEN` in a CTE, referenced as a column |

---

## Interaction with PER

WHEN composes with `PER`. When both are present, `WHEN` filters rows first, then `PER` groups the remaining rows. Each group gets its own constraint.

```sql
-- WHEN filters to 'active' rows, PER groups by department
SUM(x * hours) <= 40 WHEN status = 'active' PER department
```

Expression-level WHEN is a special case of a unified row-grouping system:

| Modifier | `row_group_ids` | `num_groups` |
|----------|-----------------|--------------|
| Neither | empty (fast path) | 0 |
| WHEN only | `0` (included) or `INVALID_INDEX` (excluded) | 1 |
| PER only | `0..K-1` (group assignment) or `INVALID_INDEX` (NULL PER value) | K |
| WHEN + PER | WHEN filters first, PER groups the rest | K (of filtered rows) |

Aggregate-local WHEN is evaluated separately from that row-grouping wrapper. Each extracted aggregate term carries its own optional filter mask. With PER, a row participates in a generated group only when it passes the global expression-level WHEN (if present), has a non-NULL PER key, and contributes to at least one aggregate-local term. Groups that end up empty after WHEN filtering are skipped — no constraint is emitted.

---

## Code Pointers

- **Grammar**: `third_party/libpg_query/grammar/statements/select.y`
  - `decide_objective_item` rule: WHEN (and WHEN+PER) support for objectives
  - `decide_constraint_item` rule: WHEN (and WHEN+PER) support for constraints
  - `func_application WHEN decide_when_condition` in `c_expr`: aggregate-local WHEN support

- **Constraint binder**: `src/planner/expression_binder/decide_constraints_binder.cpp`
  - `BindWhenConstraint()`: Extracts the WHEN condition as a separate boolean expression. Validates that the condition references only table columns, not decision variables.
  - `BindExpression()` dispatch: Recognizes top-level `WHEN_CONSTRAINT_TAG` and calls `BindWhenConstraint`; nested `WHEN_CONSTRAINT_TAG` is aggregate-local and binds through `DecideBinder::BindLocalWhenAggregate`.

- **Objective binder**: `src/planner/expression_binder/decide_objective_binder.cpp`
  - `BindExpression()`: Handles PER stripping on objectives, then WHEN condition extraction on the objective expression. Nested `WHEN_CONSTRAINT_TAG` binds as aggregate-local.
  - Objective normalization in `src/decidb/symbolic/decide_symbolic.cpp`: reassociates legacy objective comparisons like `SUM(x) WHEN flag = 'R'` back into whole-objective `WHEN(flag = 'R')`.

- **Base DECIDE binder**: `src/planner/expression_binder/decide_binder.cpp`
  - `BindLocalWhenAggregate()`: Binds the aggregate child, binds the data-only boolean condition, and stores the condition as `BoundAggregateExpression::filter`.

- **Execution**: `src/execution/operator/decide/physical_decide.cpp`
  - `AnalyzeConstraint()`: Signature takes `when_condition` and `per_columns`. PER tag is unwrapped first (outermost), then WHEN tag is unwrapped inside it.
  - `ExtractAggregateConstraintTerms()` / `ExtractAggregateObjectiveTerms()`: Extract additive aggregate expressions and copy aggregate-local filters onto linear, bilinear, and quadratic terms.
  - `Finalize()`, WHEN+PER unified grouping section: `has_when` / `has_per` flags determine evaluation path.
  - `Finalize()`, WHEN evaluation: WHEN condition evaluated into a `when_mask` boolean vector.
  - `Finalize()`, aggregate-local masks: term-level filters are evaluated and applied before row coefficients are added to the solver input.
  - `Finalize()`, row-group assignment: WHEN-only maps to group `0` or `INVALID_INDEX`; WHEN+PER filters with `when_mask` before PER grouping.

- **Data structures**: `src/include/duckdb/execution/operator/decide/physical_decide.hpp`
  - `DecideConstraint::when_condition`: Optional WHEN condition expression.
  - `DecideConstraint::per_columns`: Optional PER grouping columns (vector).
  - `Objective::per_columns`: Same for objectives.
  - `Term::filter`, `BilinearConstraintTerm::filter`, `DecideConstraint::QuadraticGroup::filter`, and `Objective::BilinearTerm::filter`: Optional aggregate-local WHEN filters carried to coefficient evaluation.

- **Evaluated constraint**: `src/include/duckdb/decidb/solver_input.hpp`
  - `EvaluatedConstraint::row_group_ids`: Per-row group assignment (`INVALID_INDEX` = excluded).
  - `EvaluatedConstraint::num_groups`: `0` = ungrouped fast path, `1` = WHEN-only, `>1` = PER groups.

- **Model builder**: `src/decidb/utility/ilp_model_builder.cpp`
  - Empty groups are skipped — no constraint is emitted.

- **Tag constants and helpers**: `src/include/duckdb/common/enums/decide.hpp`
  ```cpp
  WHEN_CONSTRAINT_TAG        = "__when_constraint__"
  PER_CONSTRAINT_TAG         = "__per_constraint__"
  IsPerConstraintTag(alias)
  ```
