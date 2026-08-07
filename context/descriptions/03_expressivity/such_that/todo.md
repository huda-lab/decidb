# SUCH THAT Clause — Planned Features

---

## `IS BETWEEN` — accept the optional `IS` before `BETWEEN`

**Priority: High — paper-facing, but small. Stage-1 grammar batch (with
`../decide/todo.md` → "Clause order", "Declaration surface syntax", "Query-wide `scalar`")
— one `make grammar-build` cycle.**

The draft spells the bounded-range constraint with an `IS`:

```sql
ship is between 0 and capacity * open        -- Figure 1, line 7
quantity is between 0 and upperBound         -- §4.1.4, knapsack example
```

Only the bare form parses today: `such that ship IS BETWEEN 0 and capacity` →
`syntax error at or near "BETWEEN"`. Everything else about the construct already works,
including a decision variable inside the bound — `such that ship BETWEEN 0 and capacity *
open` binds and solves (verified against `build/release/decidb`, 2026-08-07). So this is a
spelling, not semantics: the only gap is the optional keyword.

**Work**: accept an optional `IS` before `BETWEEN` in the decide constraint grammar. Keep
the bare form — it is what every existing test and doc example uses, and `A1`'s
both-orders decision applies here too: neither spelling is deprecated.

Scope it to the `DECIDE` constraint grammar rather than DuckDB's general `a_expr`. `IS` is
heavily overloaded in the base grammar (`IS NULL`, `IS DISTINCT FROM`, `IS NOT …`, and the
`x IS INTEGER` decide-variable declaration), so widening `a_expr` invites conflicts for a
construct only the draft's decision clauses use.

**Test**: `test/decide/tests/test_cons_between.py` — parametrize the existing cases over
both spellings and assert identical results.

**Docs on completion**: `../../00_project_overview/syntax_reference.md` §3 ("Between"),
`done.md`.

**Raised**: 2026-08-07, sweeping the submitted CIDR'27 paper against the codebase
(`../../todo.md` → A4).

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

Still deferred (separate v2 shapes, still bind-time rejected): subtraction in the
composed LHS (`MAX - MIN`), outer `WHEN`/`PER` wrappers on the composed
constraint/objective, non-constant RHS, and equality (`=`) outer comparison. Pins
for these remain `assert_error` in `test_min_max.py`; the hard-direction pins
flipped to oracle-verified positives (`test_composed_minmax_hard_max_constraint`,
`_scalar_mult_hard_min`, `_objective_hard_max`, `_objective_hard_min`).

### Related orthogonal limitation: empty-WHEN on hard-direction MIN/MAX — resolved

This used to silently produce wrong answers (the `z_k` auxiliary floated free with no linking constraints). It is now rejected before the solver runs by the empty-aggregate guard (`physical_decide.cpp:1064`), which fires on every empty aggregate regardless of shape or direction. Constraint- and objective-side coverage lives in `test_edge_cases.py`; see `05_testing/when/todo.md` → "Empty WHEN on MIN/MAX constraints — fixed and covered".

---

## NULL Coefficient Handling

**Priority: Low — requires design decision**

Currently, NULL values in constraint or objective coefficients (e.g., `SUM(x * weight)` where `weight` is NULL for some rows) produce an error:

> *"DECIDE constraint coefficient returned NULL at row N. NULL values are not allowed in optimization coefficients. Use COALESCE() to handle NULLs or filter them with WHERE clause."*

**Open question**: Should DeciDB silently treat NULL coefficients as 0 (matching SQL `SUM()` semantics where NULLs are ignored), or is requiring explicit `COALESCE()` the right design?

**Arguments for treating as 0**: SQL semantics — `SUM()` ignores NULLs. Users expect DeciDB to extend SQL naturally.

**Arguments for current behavior (error)**: NULLs in optimization coefficients are almost certainly a data quality issue. Silent coercion to 0 could hide bugs. The current error message helpfully suggests `COALESCE()`, making the user's intent explicit.
