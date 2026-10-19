# MAXIMIZE / MINIMIZE — Implemented Features

The `MAXIMIZE` or `MINIMIZE` keyword specifies the **optimization objective** — the quantity the solver should optimize while satisfying all constraints.

**Syntax** (objective forms, QP forms, bilinear, feasibility): see `../../00_project_overview/syntax_reference.md` §4. **Per-aggregate linearization** (SUM/AVG/MIN/MAX/ABS): see `../sql_functions/done.md`. **`when` in front of the objective, decisions outside reducers, `by` and the two-level form `outer(per k: inner(e) by (k))`**: see `../scope/done.md` §6. This doc covers objective-specific semantics not in those files.

---

## Requirements

1. Must be produced once: reducers (`SUM()`, `AVG()`, `MIN()`, `MAX()`, `norm()`), decisions with one instance over the kept rows (a `per ()` decision, or any decision where the `when` keeps one instance), and sums of those. A top-level reducer's `by` must be one group over the kept rows. The two-level form is the whole objective.
2. Must be **linear**, **quadratic** (`POWER(expr, 2)`), or **bilinear** (`x * y`) in the decision variables — see [problem_types/done.md](../problem_types/done.md) and [bilinear/done.md](../bilinear/done.md).
3. Must involve at least one decision variable.

The objective clause is **optional** — omitting it creates a feasibility problem (both solvers support this natively).

---

## Composed MIN/MAX in Additive Objectives

Additive objectives may mix `MIN`/`MAX` terms with `SUM`/`AVG` terms:

```sql
MAXIMIZE min(when premium_tier: x * profit) + sum(x * revenue)
MINIMIZE sum(x * cost) + max(when at_risk: x * penalty)
```

Each `MIN`/`MAX` term becomes a continuous auxiliary `z_k` pinned per-row to the inner expression; the outer sum is linear in `{x, z_k}`.

**Both directions supported.** Easy (`MAXIMIZE` with `MIN`, `MINIMIZE` with `MAX`) uses the one-sided envelope pin alone; hard (`MAXIMIZE MAX`, `MINIMIZE MIN`) adds the per-row indicator layer (binary `y_i`, `SUM(y_i) >= 1`, Big-M link) so `z_k` is pinned to the true extreme rather than floating the objective to ±∞. Scalar multiplication (`2 * MIN(...)`) also works. Same mechanism and code (`EmitExtremumLink`, `linearization_minmax.cpp`) as composed constraints — see `../such_that/done.md` → "Composed MIN/MAX (both directions)".

**A reducer's own key is carried.** A composed term may be a keyed reducer — `MINIMIZE sum(per D: opening_cost * open) + max(unit_cost * ship)`, or the colon form `SUM(D: ...)` — and the key keeps its identity semantics inside the composed clause. `ComposedMinMaxTerm::qualifier_scope_idx` is stamped by the optimizer from the binder's tag, and the physical layer ANDs the matching de-duplication mask into the term's filter mask, exactly as the non-composed reducer paths do. Applied uniformly to every term kind: for `MIN`/`MAX` it is provably a no-op (every row of an identity carries the same value, so dropping repeats cannot move an extremum), which keeps one code path rather than a special case.

**Subtraction is supported.** `MAXIMIZE max(x * v) - min(x * v)` (the spread of the selected values) works: the subtracted term carries sign `-1`, so under `MAXIMIZE` the `MIN` is pushed *down* rather than up and gets the indicator layer. The walker is shared with composed constraints, so the rule is identical — see `../such_that/done.md`.

**A `when` in front applies to every term**: `MAXIMIZE when critical: min(x * v) + sum(x * v)` runs. A decision beside a `MIN` or `MAX` is refused (*"DECIDE: 's' beside MIN or MAX is not supported yet"*).

## Quadratic Inner Expressions in the Two-Level Form

Quadratic inner expressions are supported under an outer `sum`:

```sql
MINIMIZE sum(per grp: sum(power(x - target, 2)) by (grp))   -- sum of per-group SSE; ≡ flat sum(power(...))
MINIMIZE sum(per grp: avg(power(x - target, 2)) by (grp))   -- inner avg scales each row by 1/n_g
```

Nothing expands these before binding, so the `POWER(linear, 2)` pattern reaches the QP extractor intact and the optimizer strips the outer wrapper. A `min` or `max` over a squared body is modelled as the sum of the squares, flat or two-level — a logged wrong answer (`../../06_issues/bugs/todo.md`, "A MIN/MAX over a squared or multiplied body"); `test_per_objective.py::test_maximize_sum_max_power_per` and `test_minimize_sum_min_power_per` pass only because their rows are independent. See also `../../04_testing/quadratic/done.md`.

---

## Objective and Solver Behavior

For linear objectives, the objective function is compiled into the `c` vector of the standard ILP formulation (`maximize c^T x subject to Ax <= b, x >= 0, x integer`). A `when` in front reads only the kept rows (every other row's coefficient is 0); a reducer's own `when` zeroes only that term's coefficients. For quadratic objectives, Q matrix terms are added — see [problem_types/done.md](../problem_types/done.md).

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

- **Objective binder**: `src/planner/expression_binder/decide/decide_objective_binder.cpp`
  - Validates that only `SUM`, `AVG`, `MIN`, `MAX` are used (rejects other aggregates with error message)
  - `BindScopedObjective`: binds the body, then wraps a front `when` (`WHEN_CONSTRAINT_TAG`); `per ()` adds nothing
  - `BindTwoLevelObjective`: binds `outer(per k: inner(e) by (k))` as `outer(inner(e))` under a `PER_CONSTRAINT_TAG` wrapper holding the key columns
  - A reducer's own scope and `by` bind through `DecideBinder::BindScopedReducer` / `BindReducerBy`
- **SUM argument validation**: `src/planner/expression_binder/decide/decide_binder.cpp` — `ValidateSumArgumentInternal` validates the expression tree inside SUM()
- **Two-level objective rewrite/classification**: `src/optimizer/decide/decide_rewrite_minmax.cpp`
  - `RewriteMinMaxObjective()` reads the two-level wrapper, sets the `per_inner_*` / `per_outer_*` metadata (reducer kinds, easy or hard), and rewrites inner `MIN/MAX/AVG` to `SUM`
- **Scope checks**: `DecideCanonicalizer::BuildObjectiveScopeChecks` — the front `when` keeps a row (D4); `per ()` determines every term outside reducers and every top-level `by` key
- **Solver input**: `src/include/duckdb/decidb/formulation/solver_input.hpp`
