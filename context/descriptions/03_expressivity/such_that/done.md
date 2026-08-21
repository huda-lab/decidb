# SUCH THAT Clause — Implemented Features

The `SUCH THAT` clause specifies the **constraints** of a COP query. The solver only accepts variable assignments that satisfy all constraints.

**Syntax** (operators, BETWEEN, IN, linearity rules, QCQP forms, examples): see `../../00_project_overview/syntax_reference.md` §3. **Linearization mechanics** (MIN/MAX easy/hard, ABS Path A/B, IN indicator rewrite, AVG scaling): see `../sql_functions/done.md`. **WHEN**: see `../when/done.md`. This doc covers constraint-context semantics the spec doesn't.

---

## Integer-LHS requirements for `<` / `>` / `<>`

**Strict `<` / `>` require an integer-valued LHS.** Internally `LHS < K` is rewritten to `LHS <= ceil(K) - 1`, which is only equivalent to the strict inequality when the LHS can take integer values — i.e., every referenced DECIDE variable is `INT` or `BOOL` and every coefficient is integral. Bilinear products `b * n` between a Boolean and an Integer (or Integer × Integer) count as integer-valued: the McCormick auxiliary for `b * n` is declared `INTEGER` in `decide_optimizer.cpp:RewriteBilinear`, preserving integer-valuedness through linearization. If any term makes the LHS continuous (a `REAL` variable, a fractional coefficient, or a bilinear product involving a `REAL` factor), DeciDB raises `InvalidInputException` at model-build time; use `<=` / `>=` instead. Enforced in `src/decidb/utility/ilp_model_builder.cpp` (`IsEvalConstraintLhsIntegerValued` + `ApplyComparisonSense`, plus the parallel check in `BuildQuadraticConstraint`).

**On a backend with indicator constraints, `<>` is stated rather than encoded.** Gurobi
takes `z == 0 => LHS <= K-1` and `z == 1 => LHS >= K+1` directly, so there is no Big-M
and no bound requirement — `x <> 5` answers there with an unbounded `x`. Both spellings
(per-row and aggregate) take that path, the diagnosis is unchanged (the clause is still
droppable, because an indicator constraint still carries a row), and both paths reach
the same optimum wherever both can run.

**Lowered, `<>` requires every contributing variable to be bounded.** The disjunction below is
encoded with a Big-M, and no finite constant dominates an unbounded range, so `x <> 5`
with an unbounded `x` is refused — naming `x` and the bounds to add — rather than
encoded against a large constant that could silently exclude part of the feasible
region. A bound DeciDB derives (implied-bound propagation, a declared `BOOL`'s `[0,1]`)
counts. The rule and the message are the same on both backends.

**`<>` (not-equal) also requires an integer-valued LHS.** `LHS <> K` is rewritten into the Big-M disjunction `LHS <= K-1  OR  LHS >= K+1`, which only spans the full feasible region when `LHS` can take integer values. On a REAL variable or with a non-integer coefficient the band `(K-1, K+1)` is continuous and wrongly excluded. DeciDB raises `InvalidInputException` in the same cases as strict `<` / `>`; use a reformulation such as adding an ε-band with `<=` / `>=` if the application can tolerate a small gap. Enforced at the NE expansion site in `src/execution/operator/decide/physical_decide.cpp` (covers both per-row and deferred aggregate NE paths).

**`<>` with a non-integer RHS is silently dropped (tautology).** With integer-valued LHS, `LHS <> K` for a non-integer `K` is satisfied by every integer LHS. The ±1 Big-M rewrite would wrongly exclude `floor(K)` and `ceil(K)`, so DeciDB drops such constraints rather than emitting an unsound rewrite. Three flavors:
  - **Per-row NE, uniform RHS**: whole constraint is dropped if `K` is non-integer.
  - **Per-row NE, varying RHS** (e.g. correlated subquery): only the rows whose `K_row` is non-integer are masked out (added to `row_group_ids` as `INVALID_INDEX`); the remaining rows still get the real Big-M pair.
  - **Aggregate NE (`SUM(x) <> K`, `AVG(x) <> K`)**: handled per-group in the deferred expansion. For `AVG(x) <> K` the effective per-group RHS is `K * N_g`, so groups with integer `K * N_g` get the full Big-M pair while groups whose effective RHS is non-integer are skipped (no global `z` allocated). Mixed PER queries with some groups in each category are valid. The Big-M for each group is computed by **summing the worst-case contribution over that group's rows** (the aggregate LHS ranges over the whole group); a single per-row bound would be far below the true range and silently cap the aggregate. See `../../01_pipeline/05_optimizer/done.md`.

  Enforced at `src/execution/operator/decide/physical_decide.cpp` (per-row guard inside the NE expansion loop; per-group guard inside the deferred-aggregate expansion before the `z_idx` allocation). Tolerance for the integer test is `1e-9`.

**Per-row NE indicator column storage.** When a per-row `<>` constraint is filtered by `WHEN` or grouped by `PER`, the disjunction's indicator column carries `-M` only on rows that pass the filter and `0` everywhere else — typically a small fraction of `num_rows`. The column is stored as `CoefficientColumn::SparseMasked` (a sorted list of active row indices plus a single shared value), not as a Dense `vector<double>` of mostly-zero entries. The unfiltered case stays a `Scalar` broadcast of `-M`. Defined in `src/include/duckdb/decidb/solver_input.hpp`; read paths in `src/decidb/utility/ilp_model_builder.cpp` go through `Get(row)` and observe the same zero-skip semantics as Dense.

---

## AND Separator

Constraints are joined by `AND`. Each `AND`-separated expression is a distinct constraint in the optimization model.

> **Important**: `AND` inside a `WHEN` condition has different meaning — it is part of the condition, not a constraint separator. Use parentheses: `SUM(x * w) <= 50 WHEN (cat = 'A' AND status = 'active')`.

---

## Aggregate vs. Per-Row Constraints

**Aggregate constraints** use `SUM(...)` (or `AVG`/`MIN`/`MAX`) over the relation or a filtered subset; they compile to a single linear inequality. **Per-row constraints** have no aggregate; the system generates one constraint per input row.

**Which side you write the decision on does not matter.** The binder's constraint gate
classifies *both* sides and accepts the comparison when either one is a DECIDE
expression; `DecideCanonicalizer` then moves every decision-bearing term to the left
and every data term to the right. So `5 >= x` and `x <= 5` are the same constraint, and
so are `10 >= SUM(x)` and `SUM(x) <= 10`. This happens before simple bounds are absorbed
into column bounds, so diagnostics still see the canonical bound shape.

A decision may therefore sit on the **bound** side, which is what the paper's §3.1
`max_shortfall` constraint needs:

```sql
SUCH THAT SUM(x) <= cap                       -- query-wide decision as the bound
SUCH THAT price - SUM(x) <= cap PER grp       -- paper Example 1, both halves at once
SUCH THAT SUM(x * v) <= SUM(y * v)            -- a reducer on both sides
SUCH THAT SUM(x * v) <= MAX(x * w) + 30       -- composed reducer as the bound
```

A **data** bound still has to reduce to one value per group, and that check runs on
whichever side is the bound rather than on `right` by position — so `SUM(x) <= price`
and `price >= SUM(x)` are refused identically. Homogeneity (K3) is checked further
down: a *row-scoped* decision as the bound of a reduced constraint
(`SUM(x) <= y`) is rejected by the aggregate term extractor, which names the offending
term. A query-wide (`scalar`) one is legal, because it is row-invariant.

*(History: until the canonicalization refactor the binder required the DECIDE expression on the
left and rewrote the parsed comparison to put it there. That flip was the last of five
places that decided constraint shape; deleting it is what opened the shapes above.)*

**Composed `MIN`/`MAX` in the LHS (both directions)** — an additive LHS may mix `MIN`/`MAX` terms with `SUM`/`AVG` terms. Each `MIN`/`MAX` becomes a continuous global auxiliary `z_k` pinned per row; the outer constraint is linear in `{x, z_k}`.

```sql
SUM(x * v) + MAX(x * v) <= 20                       -- easy: MAX pushed down by <=
SUM(x * v) + MAX(x * v) >= 40                       -- hard: MAX pushed up by >=
SUM(x * v) + MIN(x * v) <= 25                       -- hard: MIN pushed down by <=
SUM(x * v) + (MAX(x * v) WHEN critical) <= 20       -- aggregate-local WHEN on one term
MIN(x * v) WHEN tier_a + MIN(x * v) WHEN tier_b >= 15
(2 * MIN(x * v) WHEN w) + SUM(x * v) <= 20          -- a factor on a term; see "A Factor on a Reducer"
```

**Directions.** Every `MIN`/`MAX` term gets the same base one-sided *envelope* pin — `z_k >= inner` for `MAX`, `z_k <= inner` for `MIN`, per active row. In the **easy** direction (`MAX` pushed down by `<=`, `MIN` pushed up by `>=`) the outer pressure drives `z_k` to the extreme, so the envelope alone is exact. In the **hard** direction the outer pushes `z_k` the wrong way, so the envelope is augmented with an *indicator layer*: one binary `y_i` per active row, `SUM(y_i) >= 1`, and a Big-M link on the opposite side (`z_k <= inner_i + M(1−y_i)` for `MAX`, `z_k >= inner_i − M(1−y_i)` for `MIN`) so `z_k` is pinned to the true `MAX`/`MIN`. `M` is the signed spread of the inner expression over the term's active rows (same formula as the flat hard-`MIN`/`MAX` objective's). Emitted by `EmitComposedHardMinMaxIndicators` (`ilp_linearization.cpp`, stage 06), shared by the constraint and objective paths.

**Subtraction is supported.** `MAX(x*v) WHEN w - MIN(x*v) WHEN w <= 3` and `SUM(x) - MAX(y) <= 0` both work. A subtracted term carries sign `-1`, which flips the direction it is pushed and therefore its easy/hard classification: under `<=`, a subtracted `MAX` is pushed *up* (hard) while an added one is pushed down (easy). `WalkComposedLhs` threads the sign through binary and unary `-`; the physical layer was already sign-generic. A zero constant reaching a leaf is dropped, so a shape is never rejected purely on how its negation was spelled. `DecideOptimizer` still writes negations that way (`0 - inner_expr` in the ABS linearization); the canonicalizer no longer does — since C.2 `BuildAdditive` emits a unary minus for a leading negative term, because the synthesized `0` was a term to every downstream spine walker and is not a term to K3.

Still rejected at bind time (separate v2 shapes): outer `WHEN`/`PER` wrappers, non-constant RHS, and equality (`=`) outer comparison. The RHS test is *foldability*, not a literal node, so a bound that canonicalization rebuilt as `(0 - 3) + 0` is accepted. See also `../maximize_minimize/done.md` for composed objectives.

---

## A Factor on a Reducer

A reducer may be scaled by a factor that is **one value for the whole query** — a literal, an expression over literals, or an *uncorrelated* scalar subquery. Every spelling is accepted and they are all equivalent:

```sql
2 * SUM(x * price) <= 400            -- factor on the left
SUM(x * price) * 2 <= 400            -- factor on the right (same model)
SUM(x * price) / 2 <= 100            -- division
2 * MAX(x * v) <= 20                 -- any aggregate kind
SUM(x) + 2 * MAX(x * v) <= 30        -- inside a composed LHS
(SELECT max(budget) FROM Depts) * SUM(x) <= 1000
```

Two stages are involved, and the split is deliberate. `DecideCanonicalizer` **peels** the factor outward off the reducer and converges every spelling onto one (`scale * term`, factor on the left); it performs no algebra, so it cannot tell `SUM(x)` from `POWER(x-t,2)`. The factor then **stays outside** the reducer: the optimizer records it on the term and the physical layer multiplies it in at the end — into per-row coefficients for `SUM`/`AVG`, into the auxiliary's contribution for `MIN`/`MAX`. Nothing ever pushes it back in, because `MIN`/`MAX` are order statistics and a negative factor turns one into the other (`MAX(-2x)` is `-2·MIN(x)`).

Because the swap is what makes it exact, `MIN`/`MAX` fold only for a factor whose sign is known at plan time. `SUM` and `AVG` distribute over any factor and always fold.

**A row-varying factor is rejected**, on every aggregate kind:

```sql
weight * SUM(x) <= 4
-- DECIDE constraint: 'weight' varies per row, so it cannot multiply SUM(x).
-- Move it inside the aggregate, e.g. SUM(x * weight).
```

A reducer collapses many rows to one number, so "which row's `weight` scales it?" has no answer — the same reason plain SQL requires `col` in the `GROUP BY` for `col * SUM(x)`. The per-row-coefficient reading is still available, spelled explicitly as `SUM(weight * x)`. (This shape *was* accepted before 2026-08-10, silently rewritten to `SUM(weight * x)`.) A **decision** factor is rejected separately: `s * SUM(x)` is a product of two decisions, i.e. bilinear, not a scale.

**A correlated subquery is rejected too**, for the same reason a column is — it returns a different value for each row:

```sql
(SELECT k FROM s WHERE s.sid = t.id) * SUM(x) <= 100
-- DECIDE constraint: this subquery returns a different value for each row,
-- so it cannot multiply SUM(x). Move it inside the aggregate,
-- e.g. SUM(x * (SELECT ...)).
```

Only an **uncorrelated** scalar subquery qualifies as a factor. The message names it as a subquery rather than quoting an identifier, because flattening leaves it a column literally named `SUBQUERY`, which the user never wrote.

Telling these apart is not a shape question. After flattening, all three of `weight`, an uncorrelated subquery and a correlated subquery are plain `BoundColumnRefExpression`s on a table index of their own. So the canonicalizer is given an **allow-list**, built in `plan_select_node.cpp` at the only point where correlation is still visible: the table indexes that *uncorrelated* scalar subqueries flattened into. A column ref counts as query-wide only if it is on that list; anything unmarked is row-varying. The direction is deliberate — the reverse framing ("not one of the reduced relation's own bindings") reads identically for `weight` but silently admits a correlated subquery.

Rejected at execution time: any term whose aggregate-local `WHEN` matches zero rows — without this, the per-term `z_k` auxiliary has no pinning constraints and floats free, silently vacating the entire additive constraint. See `../when/done.md` → "Empty Row Sets" for the full rule.

---

## Subqueries in Constraints

**Uncorrelated scalar subqueries** are evaluated once and treated as a constant: `x <= (SELECT avg(budget) FROM Depts)`.

**Correlated scalar subqueries** are delegated to DuckDB's standard `ExpressionBinder`, which decorrelates them into joins via `PlanSubqueries`, producing per-row values:
- **Per-row constraints**: each row gets its own RHS value from the join — `x <= (SELECT budget FROM Depts WHERE Depts.id = items.dept_id)`.
- **Aggregate constraints**: a correlated value beside the reducer is rejected during planning because it varies per row. Move it inside the reducer when row-wise participation is intended.

A data-only *aggregate* RHS (`SUM(x*val) <= SUM(val)`, distinct from a subquery) stays on the right and is reduced to one value per group there — see "Data-Only Aggregate RHS in Aggregate Constraints" and "Reducers as a Bound" in [../sql_functions/done.md](../sql_functions/done.md). It was hoisted into the LHS before binding until 2026-08-10.

**Restriction**: Subqueries cannot reference DECIDE variables (checked at bind time via `ExpressionContainsDecideVariable`).

---

## Code Pointers

- **Constraint binder**: `src/planner/expression_binder/decide_constraints_binder.cpp`
  - `BindComparison()` — handles `=`, `<`, `<=`, `>`, `>=`, `<>` (all operators on both per-row and aggregate). Classifies both sides via `GetExpressionType` and accepts when either is a DECIDE expression (`IsDecideSide`); the constraint is reduced when either classifies as `SUM`, and only then is the non-DECIDE side checked as a bound (`IsAllowedConstraintRHS` plus decision-free). It performs no rewriting — the parsed-level side flip it used to do was deleted at the canonicalization refactor.
  - `BindBetween()` — desugars to two comparison constraints
  - `BindOperator()` — handles IN clause
  - `BindWhenConstraint()` / `BindPerConstraint()` — bind WHEN / PER modifiers; the canonicalizer validates `PER` against the final aggregate/per-row classification
  - Nested `WHEN` dispatch through `DecideBinder::BindLocalWhenAggregate()` — aggregate-local WHEN filters
  - Validates that only SUM, AVG, MIN, and MAX are used as aggregate functions
- **Subquery handling**: `src/planner/expression_binder/decide_binder.cpp`
  - `DecideBinder::BindExpression()` — validates scalar-only, no DECIDE variable references, then delegates to `ExpressionBinder::BindExpression`
  - Correlated subquery RHS validation at execution: `src/decidb/utility/ilp_model_builder.cpp`, `SolverModel::Build()`
- **Execution** (constraint matrix construction): `src/execution/operator/decide/physical_decide.cpp`
