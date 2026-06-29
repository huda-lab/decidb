# SQL Functions & Expressions — Planned Features

---


## Data-Only Aggregate RHS in Aggregate Constraints

**Planned / moderate scope.** Direct aggregate RHS expressions whose aggregate arguments do not reference DECIDE variables should be supported for aggregate constraints:

```sql
SUCH THAT SUM(x * val) <= SUM(val)
SUCH THAT AVG(x + cost) <= AVG(cost) + 1
SUCH THAT SUM(x * val) <= SUM(val) PER grp
```

Mathematically these RHS aggregates are scalar once evaluated over the relevant active row set. Today, direct RHS `SUM(...)`/`AVG(...)` reaches `TransformToChunkExpression`, which only has an internal `count_star()` aggregate special case; other RHS aggregates are rejected with `InvalidInputException`. Users can work around simple ungrouped cases by computing the bound in a scalar subquery/CTE.

Implementation notes:
- Detect and validate RHS aggregate expressions that are data-only (no DECIDE variables).
- Evaluate them with aggregate semantics rather than scalar `ExpressionExecutor` semantics.
- Match the aggregate constraint's active row set: expression-level `WHEN`, `PER`, and `AVG` denominators must use the same rows/groups as the LHS.
- Decide whether aggregate-local `WHEN` on RHS is supported or explicitly rejected.
- Let grouped aggregate constraints carry per-group RHS values instead of requiring one uniform scalar across all groups.

Keep this separate from fixed data-only terms inside the LHS aggregate body (`SUM(x + cost) <= K`), which are already supported by evaluating fixed `INVALID_INDEX` LHS terms and subtracting their active-row contribution from RHS during model building.

---

## Division (`/`) Over Decision Variables

**Not planned**. Division by a decision variable is inherently non-linear. Division by a constant is valid but can be handled by multiplying the other side (already possible with current syntax).

---

## NOT Over Decision Variable Expressions

**Not planned**. `NOT` applied to a decision variable expression would require a binary negation auxiliary variable. Use `x = 0` or `1 - x` instead.

---

## IN on Aggregates

**Not planned**. `SUM(x) IN (...)` is not supported. Use multiple equality constraints or BETWEEN instead.
