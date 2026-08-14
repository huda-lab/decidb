# Stage 02 — Binder

Resolves names, scopes and types; recognizes reducers; and decides whether a
DECIDE clause is *semantically valid*. It produces a bound representation and
nothing else — it does not move terms across a comparison, flip a relation, or
choose a solver formulation.

**Key source files**

- `src/planner/binder/query_node/bind_select_node.cpp` — declaration handling and
  the DECIDE section of `BindSelectNode`
- `src/planner/expression_binder/decide_binder.cpp` — shared DECIDE expression
  rules (reducers, degree, qualified reducers, aggregate-local `WHEN`)
- `src/planner/expression_binder/decide_constraints_binder.cpp` — `SUCH THAT`
- `src/planner/expression_binder/decide_objective_binder.cpp` — `MAXIMIZE` / `MINIMIZE`

---

## 1. Variable declarations

Each entry of `statement.decide_variables` arrives as a `PG_AEXPR_OF` comparison
pairing a `ColumnRefExpression` with a type marker string. The binder reads name,
optional table qualifier, and type, then:

| Check | Message |
|---|---|
| Name collides with a real column | `DECIDE variable '%s' conflicts with an existing column name.` |
| Name declared twice | `Duplicate DECIDE variable name '%s'.` |
| Qualifier names no table in `FROM` | `DECIDE variable '%s.%s': table '%s' not found in FROM clause.` |

Every variable is registered under its **unqualified** name; a table-scoped one is
additionally registered under `T.x` so constraints may use either spelling.

### Scope

Three scopes, recorded per variable in `variable_scopes` as a `DecideVarScopeInfo`:

| Declaration | Scope | Meaning |
|---|---|---|
| `x(TYPE)` | `Row()` | one decision per result row |
| `T.x(TYPE)` | `Entity(scope_idx)` | one decision per distinct entity in `T` |
| `scalar x(TYPE)` | `Scalar()` | one decision for the whole query |

The grammar flags the query-wide form by prefixing the type marker with
`scalar_`, which the binder strips so the type comparison stays scope-agnostic.

For a table-scoped variable, `FindOrCreateEntityScope` resolves the alias in the
bind context and either reuses or creates an `EntityScopeInfo`. The scope is keyed
by table, so several variables — and a relation-qualified reducer `SUM(T: ...)` —
share one scope, since the tuple-identity key is the same either way.
`EntityScopeInfo` carries `table_alias`, `source_table_index`,
`entity_key_bindings`, `scoped_variable_indices` and
`entity_key_column_types`; `entity_key_physical_indices` is filled in later, at
physical-plan creation.

### Types and domains

| Declaration | `var_types[i]` | `is_boolean_var[i]` |
|---|---|---|
| `x(INT)` | `LogicalType::INTEGER` | false |
| `x(BOOL)` | `LogicalType::INTEGER` | **true** |
| `x(REAL)` | `LogicalType::DOUBLE` | false |

`BOOL` is a *domain*, not a storage type. **No `x >= 0` or `x <= 1` constraint is
ever synthesized.** `is_boolean_var` carries the `[0,1]` box from here through
`LogicalDecide` to `PhysicalDecide::Finalize`, which reports the variable's type
as `LogicalType::BOOLEAN` on `SolverInput`, and the model builder applies the box
and the `is_binary` flag from the type alone
(`ilp_model_builder.cpp:192-236`). Auxiliary indicators that must stay
`INTEGER`-typed so they can appear in arithmetic like `M * z` (IN-domain and L0
indicators) use exactly the same signal.

Default bounds come from the type at model-build time: `[0, 1e30]` for INTEGER
and DOUBLE, `[0, 1]` for BOOLEAN. A user bound that widens a BOOLEAN past its
domain downgrades the column from binary to a bounded integer rather than sending
the backend a contradictory pair.

---

## 2. Degree, not occurrence count

`DecideDegreeInternal` (`decide_binder.cpp:100-137`) computes the polynomial
degree of an expression in the decision variables:

```
variable                     -> 1
cast                         -> degree(child)
+ / -                        -> max over children
*                            -> sum over children
/ (binary)                   -> degree(numerator)   (a decision-bearing divisor
                                                     is rejected separately)
POWER(base, n) / base ** n   -> degree(base) * n    (constant non-negative
                                                     integer n only)
anything else                -> occurrence count (never underestimates)
```

This matters because `(x + y) * z` has **three** occurrences but degree **2** — it
expands to `x*z + y*z`. Counting occurrences rejected it as degree > 2 in
`SUCH THAT` while accepting it in `MAXIMIZE`, because the objective path used to
be pre-expanded by a separate normalizer. Degree ≤ 2 is the gate; degree 2 is
routed to the quadratic / bilinear paths downstream.

The `POWER` case is what keeps `SUM(POWER(x,2) * y)` — genuinely degree 3 — from
passing the gate. While `POWER` fell through to occurrence counting it reported
degree 1, and the shape was refused much later by physical extraction, in
extractor vocabulary rather than as a `Binder Error`. A fractional, negative or
non-constant exponent is not a polynomial degree at all; `ValidatePowerExponent`
rejects those, so they keep the occurrence-count fallback rather than being
given a number here.

---

## 3. Reducers

`IsDecideAggregateName` recognizes `SUM`, `AVG`, `MIN`, `MAX` as DECIDE reducers.
`DecideBinder::BindAggregate` binds them; the optimizer, not the binder, decides
their formulation.

### Aggregate-local `WHEN`

`DecideBinder::BindLocalWhenAggregate` binds child 0 as a DECIDE aggregate,
validates that child 1 references no decision variable, binds it as `BOOLEAN`, and
stores it on `BoundAggregateExpression::filter`. Physical extraction copies that
filter onto each term the aggregate produces.

Dispatch: a top-level `WHEN_CONSTRAINT_TAG` is expression-level; a nested one is
aggregate-local. An expression-level `WHEN` whose child already contains an
aggregate-local `WHEN` is rejected — the double-filter semantics are ambiguous.

### Relation-qualified reducers

`DecideBinder::BindQualifiedReducer` handles `SUM(D: expr)`, which reduces over
one contribution per tuple identity of relation `D` rather than per join-output
row. `CheckQualifiedReducerBody` validates the body against that relation. The
scope is looked up through the same `FindOrCreateEntityScope` used by
table-scoped variables.

---

## 4. Constraints

`DecideConstraintsBinder` classifies each side of a comparison and type-checks it
**without moving anything**. `GetExpressionType` returns a `DecideExpression`
telling the binder whether a side is decision-bearing, and `IsSupportedComparison`
gates the operator set (`=`, `<`, `<=`, `>`, `>=`, `<>`, plus `BETWEEN` via
`BindBetween`).

The binder asks only whether *either* side bears a decision. It does not flip
`5 >= x` into `x <= 5` — the canonicalizer does that on the bound tree.

Dedicated binding methods: `BindComparison`, `BindOperator`, `BindBetween`,
`BindConjunction`, `BindWhenConstraint`, `BindPerConstraint`.

### Subqueries

Both uncorrelated and correlated **scalar** subqueries are supported, and are
delegated to DuckDB's standard `ExpressionBinder::BindExpression`. Non-scalar
subqueries are rejected, as is any subquery referencing a decision variable
(`ExpressionContainsDecideVariable`). Correlation is observed later, in
`plan_select_node.cpp`, at the only point where it is still visible — see
[`../03_logical_plan/done.md`](../03_logical_plan/done.md).

### `PER`

`BindPerConstraint` requires the `PER` expression to reference a table column —
not a decision variable, not a constant. Whether `PER` is *eligible* on this
constraint is decided after canonicalization, using the canonical aggregate /
per-row classification, not by a parsed aggregate-shape guess. Combined
expression-level `WHEN` + `PER` filters first, then groups.

---

## 5. Objective

`DecideObjectiveBinder` binds `MAXIMIZE` / `MINIMIZE` bodies through the same
`DecideBinder` rules. It knows the sense (`decide_sense`) because a few checks
depend on it. It performs no algebraic rewriting: the objective's shape — its
additive spine, its reducer scales, its constant offset — is decided once by
`DecideCanonicalizer::CanonicalizeObjective` on the bound tree, alongside
constraints.

---

## 6. Auxiliary variables

`num_user_vars` is captured before any rewrite runs, so everything appended after
it is auxiliary. Auxiliary variables are pruned from the bind context so they never
appear in `SELECT *`; they exist only in the solver's variable space.
`num_auxiliary_vars` travels to `LogicalDecide` and `PhysicalDecide`.

The binder creates no auxiliaries of its own. `norm` and `IN` bind as markers
and are lowered by the optimizer, alongside ABS, MIN/MAX, `<>` and bilinear
(stage 05). No formulation runs on the parsed tree.

---

## 7. What this stage does not do

- Move a term across a comparison or flip a relation — stage 04.
- Pick a linearization (ABS envelope, MIN/MAX easy vs hard, McCormick) — stage 05.
- Evaluate anything against data — stage 08.

---

## 8. Source map

| Concern | Location |
|---|---|
| Declarations, scopes, types, aux pruning | `src/planner/binder/query_node/bind_select_node.cpp` |
| Shared DECIDE expression rules, degree, reducers | `src/planner/expression_binder/decide_binder.cpp` |
| `SUCH THAT` binding and `PER` gate | `src/planner/expression_binder/decide_constraints_binder.cpp` |
| Objective binding | `src/planner/expression_binder/decide_objective_binder.cpp` |
| Entity scope struct | `src/include/duckdb/planner/operator/logical_decide.hpp` |
| Scope enum and DECIDE tags | `src/include/duckdb/common/enums/decide.hpp` |
