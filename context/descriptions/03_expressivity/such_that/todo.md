# SUCH THAT Clause — Planned Features

---

## Composed MIN/MAX: Hard-Direction Big-M Linearization

**Priority: Medium — feature gap, bind-time rejected today**

Composed MIN/MAX constraints — where a `MIN` or `MAX` aggregate appears as an additive term alongside `SUM` (e.g. `SUM(x*v) + MAX(x*v) <= K`) — are supported today in the **easy direction** only:

- `MAX(...) <= K` and `MIN(...) >= K` bound every row individually. The aggregate is stripped and per-row constraints are emitted. Composed forms such as `SUM + MAX <= K` also work via a per-term auxiliary `z_k` with per-row linking constraints.

The **hard direction** (`MIN(...) <= K`, `MAX(...) >= K`, equality) is rejected with:

> *"Composed MIN/MAX in DECIDE v1 supports only easy-direction MIN/MAX terms (MAX pushed down by <=, MIN pushed up by >=). The 'min' term here requires Big-M indicator linearization, which is not yet implemented for composed expressions."*

### Why this is a real gap (now more visible)

The `K * WHEN` fold in `NormalizeComparisonExpr` (see `when/done.md` — "Aggregate-local WHEN Composes with Constraint-LHS Arithmetic") rewrites shapes like `2 * (MIN(x*v) WHEN w) + SUM(x*v) <= K` into `WHEN(MIN(2*x*v), w) + SUM(x*v) <= K`. Before the fold, the composed walker's `*`/`/` arm rejected the scalar multiplier first, masking the real rejection reason. Now the fold succeeds and users hit the Big-M gap directly — with a clearer error message, but the same underlying feature gap.

### What implementing it requires

Two-level Big-M formulation, standard MILP modeling:

1. **Per-row binary indicators** for the MIN/MAX term (one `y_i` per row, `SUM(y_i) >= 1` to pin at least one row active).
2. **Global auxiliary `z_k`** capturing the MIN/MAX term's value across the additive LHS, linked to the active row's inner expression via Big-M disjunction: `z_k >= inner_i - M*(1 - y_i)` (and the symmetric bound).
3. **Big-M strategy** — constant heuristic (e.g. sum of per-row upper bounds) vs. solver-tightened. Gurobi can tighten with `addGenConstrIndicator`; HiGHS requires an explicit M.

Easy-direction composed already uses a `z_k` auxiliary (see `03_expressivity/such_that/done.md`). Hard direction extends this with the indicator layer.

### Where it lives in code

- Walker + classification: `src/optimizer/decide/decide_optimizer.cpp::WalkComposedLhs` and the `is_easy` decision per term.
- Auxiliary emission: the composed-MIN/MAX path in `src/execution/operator/decide/physical_decide.cpp` (grep `ComposedMinMaxTerm`).
- Existing hard-direction precedent (bare MIN/MAX, not composed): same file — the per-row indicator pattern used for `MIN(expr) <= K` on a non-composed LHS.

### Testing pins already in place

`test/decide/tests/test_min_max.py`:
- `test_composed_minmax_hard_rejected`
- `test_composed_minmax_objective_hard_rejected`
- `test_composed_minmax_scalar_mult_rejected` (now rejected on the Big-M direction rather than the scalar-mult direction)
- `test_composed_minmax_subtraction_rejected`
- `test_composed_minmax_per_wrapper_rejected`

See `05_testing/min_max/todo.md` for the full matrix — several of these will flip from `assert_error` to oracle-verified positive tests when the hard direction lands.

### Related orthogonal limitation: empty-WHEN on hard-direction MIN/MAX

Silently produces wrong answers today (the `z_k` auxiliary has no linking constraints and floats free). Covered in detail in `05_testing/min_max/todo.md` — blocked on the same design decision (infeasibility vs. bind-time rejection vs. silent-skip).

---

## NULL Coefficient Handling

**Priority: Low — requires design decision**

Currently, NULL values in constraint or objective coefficients (e.g., `SUM(x * weight)` where `weight` is NULL for some rows) produce an error:

> *"DECIDE constraint coefficient returned NULL at row N. NULL values are not allowed in optimization coefficients. Use COALESCE() to handle NULLs or filter them with WHERE clause."*

**Open question**: Should DecidB silently treat NULL coefficients as 0 (matching SQL `SUM()` semantics where NULLs are ignored), or is requiring explicit `COALESCE()` the right design?

**Arguments for treating as 0**: SQL semantics — `SUM()` ignores NULLs. Users expect DecidB to extend SQL naturally.

**Arguments for current behavior (error)**: NULLs in optimization coefficients are almost certainly a data quality issue. Silent coercion to 0 could hide bugs. The current error message helpfully suggests `COALESCE()`, making the user's intent explicit.
