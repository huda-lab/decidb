# SUCH THAT Clause — Implemented Features

The `SUCH THAT` clause specifies the **constraints** of a COP query. The solver only accepts variable assignments that satisfy all constraints.

**Syntax** (operators, BETWEEN, IN, linearity rules, QCQP forms, examples): see `../../00_project_overview/syntax_reference.md` §3. **Linearization mechanics** (MIN/MAX easy/hard, ABS Path A/B, IN indicator rewrite, AVG scaling): see `../sql_functions/done.md`. **WHEN**: see `../when/done.md`. This doc covers constraint-context semantics the spec doesn't.

---

## Integer-LHS requirements for `<` / `>` / `<>`

**Strict `<` / `>` require an integer-valued LHS.** Internally `LHS < K` is rewritten to `LHS <= ceil(K) - 1`, which is only equivalent to the strict inequality when the LHS can take integer values — i.e., every referenced DECIDE variable is `IS INTEGER` or `IS BOOLEAN` and every coefficient is integral. Bilinear products `b * n` between a Boolean and an Integer (or Integer × Integer) count as integer-valued: the McCormick auxiliary for `b * n` is declared `INTEGER` in `decide_optimizer.cpp:RewriteBilinear`, preserving integer-valuedness through linearization. If any term makes the LHS continuous (a `IS REAL` variable, a fractional coefficient, or a bilinear product involving a `IS REAL` factor), DeciDB raises `InvalidInputException` at model-build time; use `<=` / `>=` instead. Enforced in `src/decidb/utility/ilp_model_builder.cpp` (`IsEvalConstraintLhsIntegerValued` + `ApplyComparisonSense`, plus the parallel check in `BuildQuadraticConstraint`).

**`<>` (not-equal) also requires an integer-valued LHS.** `LHS <> K` is rewritten into the Big-M disjunction `LHS <= K-1  OR  LHS >= K+1`, which only spans the full feasible region when `LHS` can take integer values. On a REAL variable or with a non-integer coefficient the band `(K-1, K+1)` is continuous and wrongly excluded. DeciDB raises `InvalidInputException` in the same cases as strict `<` / `>`; use a reformulation such as adding an ε-band with `<=` / `>=` if the application can tolerate a small gap. Enforced at the NE expansion site in `src/execution/operator/decide/physical_decide.cpp` (covers both per-row and deferred aggregate NE paths).

**`<>` with a non-integer RHS is silently dropped (tautology).** With integer-valued LHS, `LHS <> K` for a non-integer `K` is satisfied by every integer LHS. The ±1 Big-M rewrite would wrongly exclude `floor(K)` and `ceil(K)`, so DeciDB drops such constraints rather than emitting an unsound rewrite. Three flavors:
  - **Per-row NE, uniform RHS**: whole constraint is dropped if `K` is non-integer.
  - **Per-row NE, varying RHS** (e.g. correlated subquery): only the rows whose `K_row` is non-integer are masked out (added to `row_group_ids` as `INVALID_INDEX`); the remaining rows still get the real Big-M pair.
  - **Aggregate NE (`SUM(x) <> K`, `AVG(x) <> K`)**: handled per-group in the deferred expansion. For `AVG(x) <> K` the effective per-group RHS is `K * N_g`, so groups with integer `K * N_g` get the full Big-M pair while groups whose effective RHS is non-integer are skipped (no global `z` allocated). Mixed PER queries with some groups in each category are valid. The Big-M for each group is computed by **summing the worst-case contribution over that group's rows** (the aggregate LHS ranges over the whole group); a single per-row bound would be far below the true range and silently cap the aggregate. See `../../04_optimizer/matrix_efficiency/done.md`.

  Enforced at `src/execution/operator/decide/physical_decide.cpp` (per-row guard inside the NE expansion loop; per-group guard inside the deferred-aggregate expansion before the `z_idx` allocation). Tolerance for the integer test is `1e-9`.

**Per-row NE indicator column storage.** When a per-row `<>` constraint is filtered by `WHEN` or grouped by `PER`, the disjunction's indicator column carries `-M` only on rows that pass the filter and `0` everywhere else — typically a small fraction of `num_rows`. The column is stored as `CoefficientColumn::SparseMasked` (a sorted list of active row indices plus a single shared value), not as a Dense `vector<double>` of mostly-zero entries. The unfiltered case stays a `Scalar` broadcast of `-M`. Defined in `src/include/duckdb/decidb/solver_input.hpp`; read paths in `src/decidb/utility/ilp_model_builder.cpp` go through `Get(row)` and observe the same zero-skip semantics as Dense.

---

## AND Separator

Constraints are joined by `AND`. Each `AND`-separated expression is a distinct constraint in the optimization model.

> **Important**: `AND` inside a `WHEN` condition has different meaning — it is part of the condition, not a constraint separator. Use parentheses: `SUM(x * w) <= 50 WHEN (cat = 'A' AND status = 'active')`.

---

## Aggregate vs. Per-Row Constraints

**Aggregate constraints** use `SUM(...)` (or `AVG`/`MIN`/`MAX`) over the relation or a filtered subset; they compile to a single linear inequality. **Per-row constraints** have no aggregate; the system generates one constraint per input row.

**Comparison direction is normalized at bind time.** Internally DeciDB keeps the
DECIDE-bearing expression on the left side of a comparison. User-written reversed
forms with a non-DECIDE/non-aggregate left side are flipped before validation:
`5 >= x` binds as `x <= 5`, `2 <= x` as `x >= 2`, and `10 >= SUM(x)` as
`SUM(x) <= 10`. This applies before simple bounds are absorbed into column
bounds, so diagnostics still see the canonical user bound shape.

**Composed `MIN`/`MAX` in the LHS (both directions)** — an additive LHS may mix `MIN`/`MAX` terms with `SUM`/`AVG` terms. Each `MIN`/`MAX` becomes a continuous global auxiliary `z_k` pinned per row; the outer constraint is linear in `{x, z_k}`.

```sql
SUM(x * v) + MAX(x * v) <= 20                       -- easy: MAX pushed down by <=
SUM(x * v) + MAX(x * v) >= 40                       -- hard: MAX pushed up by >=
SUM(x * v) + MIN(x * v) <= 25                       -- hard: MIN pushed down by <=
SUM(x * v) + (MAX(x * v) WHEN critical) <= 20       -- aggregate-local WHEN on one term
MIN(x * v) WHEN tier_a + MIN(x * v) WHEN tier_b >= 15
(2 * MIN(x * v) WHEN w) + SUM(x * v) <= 20          -- scalar factor folds into the inner expr
```

**Directions.** Every `MIN`/`MAX` term gets the same base one-sided *envelope* pin — `z_k >= inner` for `MAX`, `z_k <= inner` for `MIN`, per active row. In the **easy** direction (`MAX` pushed down by `<=`, `MIN` pushed up by `>=`) the outer pressure drives `z_k` to the extreme, so the envelope alone is exact. In the **hard** direction the outer pushes `z_k` the wrong way, so the envelope is augmented with an *indicator layer*: one binary `y_i` per active row, `SUM(y_i) >= 1`, and a Big-M link on the opposite side (`z_k <= inner_i + M(1−y_i)` for `MAX`, `z_k >= inner_i − M(1−y_i)` for `MIN`) so `z_k` is pinned to the true `MAX`/`MIN`. `M` is the signed spread of the inner expression over the term's active rows (same formula as the flat hard-`MIN`/`MAX` `compute_big_m`). Emitted by `EmitComposedHardMinMaxIndicators` (`physical_decide.cpp`), shared by the constraint and objective paths.

Still rejected at bind time (separate v2 shapes): subtraction (`MAX - MIN`), outer `WHEN`/`PER` wrappers, non-constant RHS, and equality (`=`) outer comparison. Scalar multiplication (`2 * MIN(...)`) now works — the symbolic `K*WHEN` fold collapses it into `MIN(2*x*v)`; a companion fix to `ExtractCoefficientWithoutVariable` stopped it dropping the nested `2` (the un-normalized `(2*x)*v` reaches the composed path directly). See also `../maximize_minimize/done.md` for composed objectives.

Rejected at execution time: any term whose aggregate-local `WHEN` matches zero rows — without this, the per-term `z_k` auxiliary has no pinning constraints and floats free, silently vacating the entire additive constraint. See `../when/done.md` → "Empty Row Sets" for the full rule.

---

## Subqueries in Constraints

**Uncorrelated scalar subqueries** are evaluated once and treated as a constant: `x <= (SELECT avg(budget) FROM Depts)`.

**Correlated scalar subqueries** are delegated to DuckDB's standard `ExpressionBinder`, which decorrelates them into joins via `PlanSubqueries`, producing per-row values:
- **Per-row constraints**: each row gets its own RHS value from the join — `x <= (SELECT budget FROM Depts WHERE Depts.id = items.dept_id)`.
- **Aggregate constraints**: the RHS must evaluate to the **same scalar for all rows** (an aggregate constraint compiles to a single inequality, requiring a single RHS). If the RHS varies per row, execution throws: "Aggregate constraint (SUM/AVG) requires a scalar right-hand side, but the RHS evaluates to different values per row".

A data-only *aggregate* RHS (`SUM(x*val) <= SUM(val)`, distinct from a subquery) is instead hoisted into the LHS before binding — see "Data-Only Aggregate RHS in Aggregate Constraints" in [../sql_functions/done.md](../sql_functions/done.md).

**Restriction**: Subqueries cannot reference DECIDE variables (checked at bind time via `ExpressionContainsDecideVariable`).

---

## Code Pointers

- **Constraint binder**: `src/planner/expression_binder/decide_constraints_binder.cpp`
  - `BindComparison()` — handles `=`, `<`, `<=`, `>`, `>=`, `<>` (all operators on both per-row and aggregate)
  - `BindBetween()` — desugars to two comparison constraints
  - `BindOperator()` — handles IN clause
  - `BindWhenConstraint()` / `BindPerConstraint()` — WHEN / PER modifiers
  - Nested `WHEN` dispatch through `DecideBinder::BindLocalWhenAggregate()` — aggregate-local WHEN filters
  - Validates that only SUM, AVG, MIN, and MAX are used as aggregate functions
- **Subquery handling**: `src/planner/expression_binder/decide_binder.cpp`
  - `DecideBinder::BindExpression()` — validates scalar-only, no DECIDE variable references, then delegates to `ExpressionBinder::BindExpression`
  - Correlated subquery RHS validation at execution: `src/decidb/utility/ilp_model_builder.cpp`, `SolverModel::Build()`
- **Execution** (constraint matrix construction): `src/execution/operator/decide/physical_decide.cpp`
