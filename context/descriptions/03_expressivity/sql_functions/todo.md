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

## Fold Data-Only Scalar *Functions* Before Symbolizing

**Planned / moderate scope.** Data-only *operators* outside the modelled set (`%`, bitwise) now fold to a per-row coefficient — see "Data-only operators the algebra doesn't model" in `done.md`. The remaining gap is arbitrary named scalar **functions** on data columns, which `ToSymbolicRecursive` still doesn't know:

```sql
SUCH THAT SUM(mod(id, 97) * x) <= 3      -- `mod(id, 97)` is data-only but function-form
SUCH THAT SUM(floor(price) * x) <= 3     -- likewise
```

The fix mirrors the operator case but at the function branch: when a scalar-function subexpression references **no DECIDE variable**, fold it to the `data_map` placeholder (`__DATA_N__`) instead of rejecting it. Two rejection sites must move together, exactly as the operator fix did:
- Binder: the unsupported-function arm of `ValidateSumArgumentInternal` (`decide_binder.cpp`) — allow data-only.
- Symbolic: the "Unsupported function" fallback in `ToSymbolicRecursive` (`decide_symbolic.cpp`, currently `InternalException`) — fold data-only to `data_map`.

Careful: keep this strictly to per-row scalar functions. Data-only *aggregates* (`SUM(AVG(price) * x)`) must NOT fold here — they need aggregate-over-active-row-set semantics (that is the "Data-Only Aggregate RHS" item above), and the coefficient evaluator can't collapse them per-row.

Discovered 2026-07-02 while landing the data-only operator fold; this is the remaining function-form increment.

---

## Division (`/`) Over Decision Variables

**Not planned**. Division by a decision variable is inherently non-linear. Division by a constant is valid but can be handled by multiplying the other side (already possible with current syntax).

---

## NOT Over Decision Variable Expressions

**Not planned**. `NOT` applied to a decision variable expression would require a binary negation auxiliary variable. Use `x = 0` or `1 - x` instead.

---

## IN on Aggregates

**Not planned**. `SUM(x) IN (...)` is not supported. Use multiple equality constraints or BETWEEN instead.
