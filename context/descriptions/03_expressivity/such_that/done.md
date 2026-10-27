# SUCH THAT Clause — Implemented Features

The `SUCH THAT` clause specifies the **constraints** of a COP query. The solver only accepts variable assignments that satisfy all constraints.

**Syntax** (operators, BETWEEN, IN, linearity rules, QCQP forms, examples): see `../../00_project_overview/syntax_reference.md` §3. **Linearization mechanics** (MIN/MAX easy/hard, ABS Path A/B, IN indicator rewrite, AVG scaling): see `../sql_functions/done.md`. **Scopes** (`when`, `per`, `by`, and how a clause becomes model rows): see `../scope/done.md`. This doc covers constraint-context semantics the syntax reference doesn't.

---

## Integer-LHS requirements for `<` / `>` / `<>`

**Strict `<` / `>` require an integer-valued LHS.** Internally `LHS < K` is rewritten to `LHS <= ceil(K) - 1`, which is only equivalent to the strict inequality when the LHS can take integer values — i.e., every referenced DECIDE variable is `INT` or `BOOL` and every coefficient is integral. Bilinear products `b * n` between a Boolean and an Integer (or Integer × Integer) count as integer-valued: the McCormick auxiliary for `b * n` is declared `INTEGER` in `decide_rewrite_bilinear.cpp:RewriteBilinear`, preserving integer-valuedness through linearization. The rule is stated on declared types and refused at bind time: a `REAL` decision by `ValidateDecideNoIntegerStepComparisonOnReal` on the parsed tree, any other operand of a fractional type (`DOUBLE`, `DECIMAL` with scale > 0, a literal like `0.5`) by `ValidateDecideIntegralComparisonOperands` on the bound tree; both messages name the offender and say to use `<=` / `>=`. A helper decision of the value rewrite is judged by the same rule (`DecideValueIsWholeNumbered`). The model builder keeps a value check for a coefficient that evaluates fractional (`IsEvalConstraintLhsIntegerValued`, `ilp_model_builder.cpp`).

**On a backend with indicator constraints, `<>` is stated rather than encoded.** Gurobi
takes `z == 0 => LHS <= K-1` and `z == 1 => LHS >= K+1` directly, so there is no Big-M
and no bound requirement — `x <> 5` answers there with an unbounded `x`. Both forms
(per-row and aggregate) take that path, the diagnosis is unchanged (the clause is still
droppable, because an indicator constraint still carries a row), and both paths reach
the same optimum wherever both can run.

**Lowered, `<>` requires every contributing variable to be bounded.** The disjunction below is
encoded with a Big-M, and no finite constant dominates an unbounded range, so `x <> 5`
with an unbounded `x` is refused — naming `x` and the bounds to add — rather than
encoded against a large constant that could silently exclude part of the feasible
region. A bound DeciDB derives (implied-bound propagation, a declared `BOOL`'s `[0,1]`)
counts. The rule and the message are the same on both backends.

**`<>` (not-equal) also requires an integer-valued LHS.** `LHS <> K` is rewritten into the Big-M disjunction `LHS <= K-1  OR  LHS >= K+1`, which only spans the full feasible region when `LHS` can take integer values. On a REAL variable or with a non-integer coefficient the band `(K-1, K+1)` is continuous and wrongly excluded. DeciDB refuses it at bind time in the same cases as strict `<` / `>`; use a reformulation such as adding an ε-band with `<=` / `>=` if the application can tolerate a small gap.

**`<>` with a non-integer RHS is silently dropped (tautology).** With integer-valued LHS, `LHS <> K` for a non-integer `K` is satisfied by every integer LHS. The ±1 Big-M rewrite would wrongly exclude `floor(K)` and `ceil(K)`, so DeciDB drops such constraints rather than emitting an unsound rewrite. Three flavors:
  - **Per-row NE, uniform RHS**: whole constraint is dropped if `K` is non-integer.
  - **Per-row NE, varying RHS** (e.g. correlated subquery): only the rows whose `K_row` is non-integer are masked out (added to `row_group_ids` as `INVALID_INDEX`); the remaining rows still get the real Big-M pair.
  - **Aggregate NE (`SUM(x) <> K`, `AVG(x) <> K`)**: handled per-group in the deferred expansion. For `AVG(x) <> K` the effective per-group RHS is `K * N_g`, so groups with integer `K * N_g` get the full Big-M pair while groups whose effective RHS is non-integer are skipped (no `z` allocated). A clause with some groups in each category is valid. The Big-M for each group is computed by **summing the worst-case contribution over that group's rows** (the aggregate LHS ranges over the whole group); a single per-row bound would be far below the true range and silently cap the aggregate. See `../../01_pipeline/05_optimizer/done.md`.
  - **A `K` that itself varies within a group (C3)**: `ExpandAggregateNotEqual`
    (`linearization_not_equal.cpp`) does not read one representative RHS value per group —
    it collects every distinct value the group's rows carry and allocates one binary
    and one Big-M pair **per distinct value**, all against the same accumulated LHS.
    The bound-side window rewrite (`RewriteRhsAggregates`, stage 05) is what makes the
    values available: it leaves a `<>` bound exactly as it arrived, rather than reducing
    it to the group's tightest value the way it does for `<=`/`>=`. The integer-tautology guard and the
    range collapse both run per value, so one excluded value in a group can drop as a
    tautology (or collapse to a plain inequality) while another in the same group
    keeps its full disjunction.

  Enforced at `src/execution/operator/decide/physical_decide.cpp` (per-row guard inside the NE expansion loop; per-group guard inside the deferred-aggregate expansion before the `z_idx` allocation). Tolerance for the integer test is `1e-9`.

**Per-row NE indicator column storage.** When a per-row `<>` constraint reads only some rows — a `when` in front, or one row per class under `per K:` — the disjunction's indicator column carries `-M` only on rows that pass the filter and `0` everywhere else — typically a small fraction of `num_rows`. The column is stored as `CoefficientColumn::SparseMasked` (a sorted list of active row indices plus a single shared value), not as a Dense `vector<double>` of mostly-zero entries. The unfiltered case stays a `Scalar` broadcast of `-M`. Defined in `src/include/duckdb/decidb/formulation/solver_input.hpp`; read paths in `src/decidb/formulation/ilp_model_builder.cpp` go through `Get(row)` and observe the same zero-skip semantics as Dense.

---

## AND Separator

Constraints are joined by `AND`. Each `AND`-separated expression is a distinct constraint in the optimization model.

An `and` inside a `when` condition belongs to the condition: everything up to the `per` or the `:` is the condition, so `when cat = 'A' and status = 'active': sum(x * w) <= 50` is one constraint and needs no parentheses. An `OR` between constraints, and a comma between them, are refused.

---

## Aggregate vs. Per-Row Constraints

**Per-row constraints** have no decision reducer; the system generates one constraint per kept row, or one per class under `per K:`. **Aggregate constraints** use `SUM(...)` (or `AVG`/`MIN`/`MAX`) over the kept rows a `by` selects; when their decision reducers share one `by` key with only query-wide terms beside them they compile to one linear row per `by` group, and otherwise each reducer becomes a helper decision and the clause is one row per class (the shapes are in `../scope/done.md` §3).

**Which side you write the decision on does not matter.** The binder's constraint gate
classifies *both* sides and accepts the comparison when either one is a DECIDE
expression; `DecideCanonicalizer` then moves every decision-bearing term to the left
and every data term to the right. So `5 >= x` and `x <= 5` are the same constraint, and
so are `10 >= SUM(x)` and `SUM(x) <= 10`. This happens before simple bounds are folded
into column bounds, so diagnostics still see the canonical bound shape.

A decision may therefore sit on the **bound** side, which is what the paper's §3.1
`max_shortfall` constraint needs:

```sql
SUCH THAT sum(x) <= cap                              -- query-wide decision as the bound
SUCH THAT per grp: price - sum(x) by (grp) <= cap    -- paper Example 1, both halves at once
SUCH THAT sum(x * v) <= sum(y * v)                   -- a reducer on both sides
SUCH THAT sum(x * v) <= max(x * w) + 30              -- composed reducer as the bound
```

A **data** bound with no `per` reduces to one value per `by` group — the tightest of
its own rows' values for `<=`/`<`/`>=`/`>` (paper §3.2.1) — and that check runs on
whichever side is the bound rather than on `right` by position, so
`sum(x) by (depotID) <= stock` and `stock >= sum(x) by (depotID)` build the same model.
`IsAllowedDecisionFreeBoundExpression` (`decide_constraints_binder.cpp`) accepts a plain
column here (C1) exactly as it accepts a scalar subquery — a bare column is just another
decision-free expression, and the bound-side window rewrite (`RewriteRhsAggregates`), which
reduces it per group, does not care where its per-row value came from. `=` refuses a bound that genuinely varies (a
contradiction: two different single numbers can't both be *the* sum); `<>` instead
keeps every value it took (C3) — see "Reducers as a Bound" in
[`../sql_functions/done.md`](../sql_functions/done.md). Under an explicit `per K:` the
bound must instead be determined by `K` (`per grp: price - ...` needs one `price` per
`grp`). A query-wide (`per ()`) decision as the bound keeps the aggregate shape, because
it is row-invariant; a per-row or keyed one (`sum(x) <= y`) takes the value shape — the
reducer becomes a helper decision and each row bounds it by its own `y`.

**Composed `MIN`/`MAX` in the LHS (both directions)** — an additive LHS may mix `MIN`/`MAX` terms with `SUM`/`AVG` terms. Each `MIN`/`MAX` becomes a continuous global auxiliary `z_k` pinned per row; the outer constraint is linear in `{x, z_k}`.

```sql
sum(x * v) + max(x * v) <= 20                       -- easy: max pushed down by <=
sum(x * v) + max(x * v) >= 40                       -- hard: max pushed up by >=
sum(x * v) + min(x * v) <= 25                       -- hard: min pushed down by <=
sum(x * v) + max(when critical: x * v) <= 20        -- a filter on one term
min(when tier = 1: x * v) + min(when tier = 2: x * v) >= 15
2 * min(when w: x * v) + sum(x * v) <= 20           -- a factor on a term; see "A Factor on a Reducer"
```

**Directions.** Every `MIN`/`MAX` term gets the same base one-sided *envelope* pin — `z_k >= inner` for `MAX`, `z_k <= inner` for `MIN`, per active row. In the **easy** direction (`MAX` pushed down by `<=`, `MIN` pushed up by `>=`) the outer pressure drives `z_k` to the extreme, so the envelope alone is exact. In the **hard** direction the outer pushes `z_k` the wrong way, so the envelope is augmented with an *indicator layer*: one binary `y_i` per active row, `SUM(y_i) >= 1`, and a Big-M link on the opposite side (`z_k <= inner_i + M(1−y_i)` for `MAX`, `z_k >= inner_i − M(1−y_i)` for `MIN`) so `z_k` is pinned to the true `MAX`/`MIN`. `M` is the signed spread of the inner expression over the term's active rows (same formula as the flat hard-`MIN`/`MAX` objective's). Emitted by `EmitExtremumLink` (`linearization_minmax.cpp`, stage 06), the single emitter every MIN/MAX auxiliary in the model goes through — a composed term is the case that needs *both* sides of the link, because a constraint supplies no optimization pressure to pin `z_k` with.

**Subtraction is supported.** `max(when w: x * v) - min(when w: x * v) <= 3` and `sum(x) - max(y) <= 0` both work. A subtracted term carries sign `-1`, which flips the direction it is pushed and therefore its easy/hard classification: under `<=`, a subtracted `MAX` is pushed *up* (hard) while an added one is pushed down (easy). `WalkComposedLhs` threads the sign through binary and unary `-`; the physical layer is sign-generic. A zero constant reaching a leaf is dropped, so a shape is never rejected purely on how its negation was spelled. `DecideOptimizer` writes negations that way (`0 - inner_expr` in the ABS linearization); the canonicalizer does not — `BuildAdditive` emits a unary minus for a leading negative term.

**Equality is supported.** `SUM(x*v) + MAX(x*v) = K` holds the composed sum from
both sides at once, so no term in it has an easy direction: every `z_k` takes the
envelope *and* the closing indicator layer, and the outer row reaches the backend
as one native `=` rather than a pair of opposing inequalities. `WalkComposedLhs`
expresses this as `exact_pin`, which short-circuits the easy/hard classification
the same way an unknown factor sign does. `BETWEEN` reaches the composed path as
two directional halves and solves; equal endpoints therefore also work, and agree
with `= K` — but as two comparisons they allocate two auxiliaries and pay for the
indicator layer on one of them anyway, so `= K` is the cheaper spelling of the
same constraint.

A `when` in front applies to every term (`when critical: sum(x * v) + max(x * v) <=
20`). With a `by`, a bound that is not a constant, or a query-wide decision beside it,
the clause takes the value shape instead: each `MIN`/`MAX` becomes a helper decision
pinned per group (`../scope/done.md` §3). The constant test is *foldability*, not a
literal node, so a bound that canonicalization rebuilt as `(0 - 3) + 0` keeps the
composed path. A `<>` comparison is refused (*"Composed MIN/MAX in DECIDE v1 supports
only the <, <=, =, >, >= and BETWEEN comparisons."*), as is `<>` on a lone `MIN`/`MAX`
(*"DECIDE does not support <> comparison with MIN/MAX aggregates."*). See also
`../maximize_minimize/done.md` for composed objectives.

---

## A Factor on a Reducer

A reducer may be scaled by a factor that is **one value for the whole query** — a literal, an expression over
literals, or an *uncorrelated* scalar subquery. Every spelling is accepted and they are all equivalent:

```sql
2 * sum(x * price) <= 400            -- factor on the left
sum(x * price) * 2 <= 400            -- factor on the right (same model)
sum(x * price) / 2 <= 100            -- division
2 * max(x * v) <= 20                 -- any aggregate kind
sum(x) + 2 * max(x * v) <= 30        -- inside a composed LHS
(SELECT max(budget) FROM Depts) * sum(x) <= 1000
```

Two stages are involved, and the split is deliberate. `DecideCanonicalizer` **peels** the factor outward off the
reducer and converges every spelling onto one (`scale * term`, factor on the left); it performs no algebra, so it
cannot tell `SUM(x)` from `POWER(x-t,2)`. The factor then **stays outside** the reducer: the optimizer records it on
the term and the physical layer multiplies it in at the end — into per-row coefficients for `SUM`/`AVG`, into the
auxiliary's contribution for `MIN`/`MAX`. Nothing ever pushes it back in, because `MIN`/`MAX` are order statistics and a
negative factor turns one into the other (`MAX(-2x)` is `-2·MIN(x)`).

Because the swap is what makes it exact, `MIN`/`MAX` fold only for a factor whose sign is known at plan time. `SUM` and `AVG` distribute over any factor and always fold.

**A row-varying factor is a tuple term of the constraint.** `weight * sum(x) <= 4` is one constraint per kept row, each scaling the one total by that row's `weight`: the reducer becomes a helper decision (the value shape, `../scope/done.md` §3) and each row is `weight * helper <= 4`. A correlated subquery as the factor is the same (`(SELECT k FROM s WHERE s.sid = t.id) * sum(x) <= 12`). Under `per K:` the key must determine the factor (*"DECIDE: per grp does not determine w. ..."*). The per-row-coefficient reading is a different constraint, spelled `sum(weight * x)`.

**In the objective a row-varying factor is refused**, since the objective is produced once and no row's value can scale it:

```sql
MAXIMIZE weight * sum(x)
-- DECIDE objective: 'weight' varies per row, so it cannot multiply SUM(x).
-- Move it inside the aggregate, e.g. SUM(x * weight).
MAXIMIZE (SELECT k FROM s WHERE s.sid = t.id) * sum(x)
-- DECIDE objective: this subquery returns a different value for each row,
-- so it cannot multiply SUM(x). Move it inside the aggregate,
-- e.g. SUM(x * (SELECT ...)).
```

The message names a correlated subquery as a subquery rather than quoting an identifier, because flattening leaves it a column literally named `SUBQUERY`, which the user never wrote.

**A decision factor is refused** in a constraint and in the objective: `c * sum(x)` is a product of two decisions, not a scale (`PeelScale`: *"DECIDE constraint: 'c' is a decision, so it cannot multiply SUM(x). Only data can scale SUM/AVG/MIN/MAX."*).

Telling a whole-query factor from a row-varying one is not a shape question. After flattening, all three of `weight`, an uncorrelated subquery and a correlated subquery are plain `BoundColumnRefExpression`s on a table index of their own. So the canonicalizer is given an **allow-list**, built in `plan_select_node.cpp` at the only point where correlation is still visible: the table indexes that *uncorrelated* scalar subqueries flattened into. A column ref counts as query-wide only if it is on that list; anything unmarked is row-varying, which sends a constraint to the value shape and refuses an objective. The direction is deliberate — the reverse framing ("not one of the reduced relation's own bindings") reads identically for `weight` but silently admits a correlated subquery.

A term whose own `when` leaves a `by` group with no row is an error before solving (*"DECIDE: max(when tier = 9: x * v) has no rows."*, D8, `../scope/done.md` §4); without it a `MIN`/`MAX` term's auxiliary would have no pinning rows and float free.

---

## Subqueries in Constraints

**Uncorrelated scalar subqueries** are evaluated once and treated as a constant: `x <= (SELECT avg(budget) FROM Depts)`.

**Correlated scalar subqueries** are delegated to DuckDB's standard `ExpressionBinder`, which decorrelates them into joins via `PlanSubqueries`, producing per-row values:
- **Per-row constraints**: each row gets its own RHS value from the join — `x <= (SELECT budget FROM Depts WHERE Depts.id = items.dept_id)`.
- **Beside a reducer**: a correlated value is a row-varying bound like a column — with no `per`, the tightest value per `by` group (`sum(x) <= (SELECT k FROM s WHERE s.sid = t.id)`); under `per K:` it must be determined by `K`. As a factor on a reducer it takes the value shape (above).

A data-only *aggregate* bound (`sum(x * val) <= sum(val)`, distinct from a subquery) stays on the right and is reduced to one value per group there — see "Data-Only Aggregate RHS in Aggregate Constraints" and "Reducers as a Bound" in [../sql_functions/done.md](../sql_functions/done.md).

**Restriction**: Subqueries cannot reference DECIDE variables (checked at bind time via `ExpressionContainsDecideVariable`).

---

## NULL Values in Constraint and Objective Expressions

A NULL reaching any number the solver needs is an error, not a zero. This covers
every value DECIDE evaluates from data: constraint coefficients, objective
coefficients, quadratic and bilinear coefficients, constraint right-hand sides,
and the input to a reducer used as a bound.

**Why an error rather than SQL's `SUM()` behaviour.** SQL's `SUM()` skips NULLs,
so treating a NULL coefficient as 0 looks like the natural extension. It is not,
for three reasons.

- A NULL coefficient is nearly always a data-quality problem. Coercing it to 0
  silently optimizes a different problem than the one written, and the answer
  looks perfectly ordinary — there is no later point at which the substitution
  becomes visible.
- Coefficients and bounds would need different rules. A NULL coefficient read as
  0 means "this row contributes nothing", which is at least arguable. A NULL
  bound read as 0 turns `SUM(x) <= budget` into `SUM(x) <= 0`, which forbids
  everything and reports the query infeasible for a reason the user cannot see.
- It would not actually match SQL. `AVG(expr) op K` is evaluated as
  `SUM(expr) op K*N` with `N` the row count, while SQL's `AVG` drops NULL rows
  from the count as well as the sum. Matching SQL would mean adjusting `N` per
  group, so "just follow `SUM()`" does not describe a single coherent rule.

**What the error says.** It names the column that is NULL and the two ways out:

```
DECIDE: column "weight" is NULL. Impute it with COALESCE(weight, 0) or filter
those rows out with a WHERE clause.
```

`COALESCE` and its `IFNULL` alias are accepted inline wherever a data value is
used, including the common reducer form `SUM(keep * COALESCE(weight, 0))` and
the corresponding objective. The imputation must be decision-free:
`COALESCE(keep, 0)` is rejected because `keep` is controlled by the solver.

The column is identified on the error path by walking the coefficient expression
for its leaf references and testing each against the failing row, so a NULL
inside a compound coefficient such as `SUM(x * (weight / capacity))` still names
`weight` rather than the whole expression. Where several are NULL on the same row
all are named. A NULL that no column explains — one an operation produced — has
no name to give, so the expression is quoted instead.

No row number is reported. The fix is applied to the column, not to a row, and a
scan ordinal is not something the user can look up in their own data.

---

## Code Pointers

- **Constraint binder**: `src/planner/expression_binder/decide/decide_constraints_binder.cpp`
  - `BindComparison()` — handles `=`, `<`, `<=`, `>`, `>=`, `<>` (all operators on both per-row and aggregate). Classifies both sides via `GetExpressionType` and accepts when either is a DECIDE expression (`IsDecideSide`); the constraint is reduced when either classifies as `SUM`, and only then is the non-DECIDE side checked as a bound (`IsAllowedDecisionFreeBoundExpression`). It performs no rewriting.
  - `BindBetween()` — desugars to two comparison constraints
  - `BindOperator()` — handles IN clause
  - `BindScopedConstraint()` — binds `[when c] [per K]:` in front of a constraint into the `WHEN_CONSTRAINT_TAG` / `PER_CLASS_TAG` wrappers; a reducer's own scope and `by` bind through `DecideBinder::BindScopedReducer()` / `BindReducerBy()` (`../scope/done.md` §10)
  - `BindConjunction()` — `AND` between constraints; an `OR` between them is refused
  - Validates that only SUM, AVG, MIN, and MAX are used as aggregate functions
- **Subquery handling**: `src/planner/expression_binder/decide/decide_binder.cpp`
  - `DecideBinder::BindExpression()` — validates scalar-only, no DECIDE variable references, then delegates to `ExpressionBinder::BindExpression`
  - Correlated subquery RHS validation at execution: `src/decidb/formulation/ilp_model_builder.cpp`, `SolverModel::Build()`
- **Execution** (constraint matrix construction): `src/execution/operator/decide/physical_decide.cpp`
  - `ExtractDoubleColumn()` — the single NULL / non-finite gate every evaluated coefficient and bound passes through
  - `CollectNullColumnsAtRow()` / `DescribeNullSource()` — name the NULL column on the error path
- **Tests**: `test/decide/tests/test_null_coalesce_reducer.py` — inline reducer imputation in constraints and
  objectives, the `IFNULL` alias, oracle agreement, and decision-bearing rejection
