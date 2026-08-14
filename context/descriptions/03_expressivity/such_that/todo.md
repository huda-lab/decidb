# SUCH THAT Clause — Planned Features

---

## Composed MIN/MAX: Hard-Direction Big-M Linearization — SHIPPED

Hard-direction composed MIN/MAX (`SUM + MAX >= K`, `SUM + MIN <= K`, and the
objective forms `MAXIMIZE + MAX` / `MINIMIZE + MIN`) is now implemented for both
constraints and objectives. See `done.md` → "Composed MIN/MAX (both directions)".
The base one-sided envelope pin (already emitted for both directions) is
augmented, for a hard term, with a per-row binary `y_i`, `SUM(y_i) >= 1`, and a
Big-M link (`EmitComposedHardMinMaxIndicators` in `physical_decide.cpp`); the M
is the signed spread of the inner expression, shared with the flat hard-MIN/MAX
`compute_big_m`. A latent bug in the same landing — `ExtractCoefficientWithoutVariable`
dropped a nested scalar factor like the `2` in `(2*x)*v` (which reaches the
composed path un-normalized), silently wrong for both easy and hard composed
scalar terms — was fixed as part of it.

Still deferred (separate v2 shapes, still bind-time rejected): outer `WHEN`/`PER`
wrappers on the composed constraint/objective, non-constant RHS, and equality
(`=`) outer comparison. Pins for these remain `assert_error` in
`test_min_max.py`; the hard-direction pins flipped to oracle-verified positives
(`test_composed_minmax_hard_max_constraint`, `_scalar_mult_hard_min`,
`_objective_hard_max`, `_objective_hard_min`). Subtraction in the composed LHS
(`MAX - MIN`) shipped with the canonicalization sign-awareness work — see
`done.md` and `test_canonicalize_sign.py`.

### Related orthogonal limitation: empty-WHEN on hard-direction MIN/MAX — resolved

This used to silently produce wrong answers (the `z_k` auxiliary floated free with no linking constraints). It is now rejected before the solver runs by the empty-aggregate guard (`physical_decide.cpp:1064`), which fires on every empty aggregate regardless of shape or direction. Constraint- and objective-side coverage lives in `test_edge_cases.py`; see `04_testing/when/todo.md` → "Empty WHEN on MIN/MAX constraints — fixed and covered".

---

## NULL Coefficient Handling

**Priority: Low — requires design decision**

Currently, NULL values in constraint or objective coefficients (e.g., `SUM(x * weight)` where `weight` is NULL for some rows) produce an error:

> *"DECIDE constraint coefficient returned NULL at row N. NULL values are not allowed in optimization coefficients. Use COALESCE() to handle NULLs or filter them with WHERE clause."*

**Open question**: Should DeciDB silently treat NULL coefficients as 0 (matching SQL `SUM()` semantics where NULLs are ignored), or is requiring explicit `COALESCE()` the right design?

**Arguments for treating as 0**: SQL semantics — `SUM()` ignores NULLs. Users expect DeciDB to extend SQL naturally.

**Arguments for current behavior (error)**: NULLs in optimization coefficients are almost certainly a data quality issue. Silent coercion to 0 could hide bugs. The current error message helpfully suggests `COALESCE()`, making the user's intent explicit.
