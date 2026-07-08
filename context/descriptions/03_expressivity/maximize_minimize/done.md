# MAXIMIZE / MINIMIZE — Implemented Features

The `MAXIMIZE` or `MINIMIZE` keyword specifies the **optimization objective** — the quantity the solver should optimize while satisfying all constraints.

**Syntax** (objective forms, QP forms, bilinear, feasibility): see `../../00_project_overview/syntax_reference.md` §4. **Per-aggregate linearization** (SUM/AVG/MIN/MAX/ABS): see `../sql_functions/done.md`. **WHEN on objectives**: see `../when/done.md`. **PER on objectives** (nested-aggregate two-level formulation): see `../per/done.md`. This doc covers objective-specific semantics not in those files.

---

## Requirements

1. Must use a supported aggregate (`SUM()`, `AVG()`, `MIN()`, `MAX()`) or an additive expression of supported aggregate terms.
2. Must be **linear**, **quadratic** (`POWER(expr, 2)`), or **bilinear** (`x * y`) in the decision variables — see [problem_types/done.md](../problem_types/done.md) and [bilinear/done.md](../bilinear/done.md).
3. Must involve at least one decision variable.

The objective clause is **optional** — omitting it creates a feasibility problem (both solvers support this natively).

---

## Composed MIN/MAX in Additive Objectives

Additive objectives may mix `MIN`/`MAX` terms with `SUM`/`AVG` terms:

```sql
MAXIMIZE MIN(x * profit) WHEN premium_tier + SUM(x * revenue)
MINIMIZE SUM(x * cost) + MAX(x * penalty) WHEN at_risk
```

Each `MIN`/`MAX` term becomes a continuous auxiliary `z_k` pinned per-row to the inner expression; the outer sum is linear in `{x, z_k}`.

**Both directions supported.** Easy (`MAXIMIZE` with `MIN`, `MINIMIZE` with `MAX`) uses the one-sided envelope pin alone; hard (`MAXIMIZE MAX`, `MINIMIZE MIN`) adds the per-row indicator layer (binary `y_i`, `SUM(y_i) >= 1`, Big-M link) so `z_k` is pinned to the true extreme rather than floating the objective to ±∞. Scalar multiplication (`2 * MIN(...)`) also works. Same mechanism and code (`EmitComposedHardMinMaxIndicators`) as composed constraints — see `../such_that/done.md` → "Composed MIN/MAX (both directions)".

**Still rejected at bind time** (v2): subtraction in the additive sum (`MAX - MIN`), outer `PER`/`WHEN` wrapper on the composed objective.

## Quadratic Inner Expressions Under Nested PER

Quadratic inner expressions are supported under nested outer-`SUM`:

```sql
MINIMIZE SUM(SUM(POWER(x - target, 2))) PER grp   -- sum per-group SSE; ≡ flat SUM(POWER(...))
MINIMIZE SUM(AVG(POWER(x - target, 2))) PER grp   -- inner AVG scales each row by 1/n_g
```

These forms are detected by `SumInnerIsQuadratic` (`src/decidb/symbolic/decide_symbolic.cpp`), which preserves the raw AST through normalization so the post-bind optimizer can strip the outer wrapper. `SUM(MIN(POWER(...))) PER grp` and `SUM(MAX(POWER(...))) PER grp` also bind, but physical-layer correctness for the quadratic per-row auxiliary path is tracked separately in `../../05_testing/quadratic/todo.md`.

---

## Objective and Solver Behavior

For linear objectives, the objective function is compiled into the `c` vector of the standard ILP formulation (`maximize c^T x subject to Ax <= b, x >= 0, x integer`). With an expression-level `WHEN`, coefficients for non-matching rows are set to 0; with aggregate-local `WHEN`, only that aggregate term's coefficients are zeroed. For quadratic objectives, Q matrix terms are added — see [problem_types/done.md](../problem_types/done.md).

---

## Use Cases

| Task | Typical Objective |
|---|---|
| Knapsack / subset selection | `MAXIMIZE SUM(x * value)` |
| Minimize items selected | `MINIMIZE SUM(x)` |
| Active learning (minimize uncertainty) | `MINIMIZE SUM(keep * confidence)` |
| Maximize retained data after cleaning / outlier removal | `MAXIMIZE SUM(keep)` |
| Entity resolution / deduplication | `MAXIMIZE SUM(keepS) + SUM(keepP)` |
| Data repair / imputation (L1) | `MINIMIZE SUM(ABS(new_val - old_val))` |
| Data repair / imputation (L2) | `MINIMIZE SUM(POWER(new_val - old_val, 2))` |

---

## Code Pointers

- **Objective binder**: `src/planner/expression_binder/decide_objective_binder.cpp`
  - Validates that only `SUM`, `AVG`, `MIN`, `MAX` are used (rejects other aggregates with error message)
  - Handles WHEN condition extraction on objective
  - Dispatches nested `WHEN` on aggregate terms to aggregate-local binding
  - Binds nested aggregate PER objectives (inner/outer aggregate detection)
- **SUM argument validation**: `src/planner/expression_binder/decide_binder.cpp` — `ValidateSumArgumentInternal` validates the expression tree inside SUM()
- **Nested aggregate PER objective rewrite/classification**: `src/optimizer/decide/decide_optimizer.cpp`
  - `RewriteMinMaxObjective()` detects `OUTER(INNER(expr)) PER col`, sets `per_inner_*` / `per_outer_*` metadata, and rewrites inner `MIN/MAX/AVG` to `SUM`
  - Rejects flat `MIN(...) PER col` / `MAX(...) PER col` as ambiguous (requires nested aggregate form)
- **Solver input**: `src/include/duckdb/decidb/solver_input.hpp`
