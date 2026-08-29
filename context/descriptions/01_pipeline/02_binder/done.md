# Stage 02 — Binder

Resolves names, scopes and types; recognizes reducers; and decides whether a
DECIDE clause is *semantically valid*. It produces a bound representation and
nothing else — it does not move terms across a comparison, flip a relation, or
choose a solver formulation.

**Key source files**

- `src/planner/binder/query_node/bind_select_node.cpp` — declaration handling and
  the DECIDE section of `BindSelectNode`
- `src/planner/expression_binder/decide/decide_binder.cpp` — shared DECIDE expression
  rules (reducers, degree, qualified reducers, aggregate-local `WHEN`)
- `src/planner/expression_binder/decide/decide_constraints_binder.cpp` — `SUCH THAT`
- `src/planner/expression_binder/decide/decide_objective_binder.cpp` — `MAXIMIZE` / `MINIMIZE`

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
| `x(INT)` | `LogicalType::BIGINT` | false |
| `x(BOOL)` | `LogicalType::INTEGER` | **true** |
| `x(REAL)` | `LogicalType::DOUBLE` | false |

`INT` is **`BIGINT`, not `INTEGER`**. This is the only layer that fixes the width
of a decision's result column, and it must choose one wide enough for any value
the solve can legitimately reach — which is not something a bound can tell it. A
decision's real limit may arrive as a column read at execution (`x <= cap`), or as
an aggregate row that bounds no single variable at all (`SUM(x) <= 5000000000`),
so there is nothing here to inspect and range-check. Picking a 32-bit column
instead pushed the failure to readback, where the excess was silently truncated
and the type's limit returned as the answer. `BIGINT` also matches what DuckDB
returns for generated integers (`range()`). `BOOL` stays `INTEGER`: its domain is
`0`/`1`, so it cannot overflow, and widening it would move a result column type
for no gain. Readback still range-checks both (stage 08, `Type-specific
projection`) — a double stops counting consecutively past `2^53`, which no
integer width fixes.

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

Degree is one concept with one owner, and this stage is it.
`DecideExpressionDegree` (`decide_degree.cpp`) is the only implementation in
DeciDB. It reads the **bound** tree and returns a `DecideDegree`: the polynomial
degree in decision variables, plus — at degree 2 — which of the two supported
shapes produced it.

```
decision column              -> 1
data column / constant       -> 0
cast                         -> degree(child)
reducer (SUM/AVG/MIN/MAX)    -> degree(argument)   (combines rows, does not
                                                    multiply them)
ABS(e)                       -> degree(e)          (linearized at stage 05;
                                                    changes magnitude, not degree)
+ / -                        -> max over children
*                            -> sum over children
/ (binary)                   -> degree(numerator)  (a decision-bearing divisor
                                                    is unclassifiable)
POWER(base, n) / base ** n   -> degree(base) * n   (constant non-negative
                                                    integer n only)
anything else over a decision -> unclassifiable (refused)
```

Degree is not an occurrence count: `(x + y) * z` has **three** occurrences but
degree **2**, because it expands to `x*z + y*z`. Degree ≤ 2 is the gate. At
degree 2, `is_quadratic_form` separates the two formulations — true when the
degree came from squaring (`x*x`, `POWER(x, 2)`, `POWER(x+y, 2)`), which feeds a
Q matrix, and false when two *different* decisions were multiplied (`x*y`),
which feeds McCormick envelopes.

`ValidateDecideConstraintDegree` and `ValidateDecideObjectiveDegree` run in
`bind_select_node.cpp` immediately after the constraint and objective binders,
and they are **total**: the constraint validator descends conjunctions and
`WHEN` / `PER` wrappers to every comparison that becomes a model row, and checks
both sides. That totality is the point. The rule previously lived inside
`ValidateSumArgumentInternal`, whose name scoped it to reducer arguments, so
nothing ever applied it to a bare per-row constraint — `SUM(POWER(x*y,2)) <= 10`
was a `Binder Error` while the identical `POWER(x*y,2) <= 5` bound cleanly and
was refused at plan time by term extraction, in extractor vocabulary and with no
source location. Which layer refused depended on how the user spelled it.

**Why the judgement stays here rather than moving to stage 05**, which is the
only stage that can see the tree after the nine rewrites have run: validity must
follow from the query and the schema, exactly as the integrality gate does
(§4) — whether `POWER(x*y,2) <= 5` is a legal DECIDE query cannot depend on what
`RewriteInDomain` did to it. And only a bind-time refusal can point at the term:
a `BinderException` raised here carries a caret to the offending product, which
a plan-time `InvalidInputException` has no way to do.

Stage 05 consumes the same function to assert that its own rewrites preserved
what was admitted — it never decides. See
[`../05_optimizer/done.md`](../05_optimizer/done.md) §1a.

### Bound nodes carry a source location

The DECIDE binders dispatch through `BindExpression`, which bypasses the
propagation `ExpressionBinder::Bind` does at its own entry point, so the bound
tree used to reach the post-binding validators with no location at all.
`DecideBinder::PreserveQueryLocation` now stamps it in each `BindExpression`
override — filling only an empty location, so an inner node keeps its own span
rather than being widened to its parent's. The degree gate needs this to point a
caret, and the integrality gate's two refusals (§4) gained one as a side effect;
they had been passing no node to `BinderException` because there was nothing
useful to pass.

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

A reducer may name more than one relation, `SUM(D, T: expr)` (batch E). The
composite scope's tuple identity is the concatenation of every named relation's
own key — the same "all columns of the table" key a single-relation scope
already uses, just for each relation in the list — so a row is a duplicate only
when it repeats on **every** named relation's key at once. Concretely: naming a
relation removes the fan-out it would otherwise contribute; a relation left
unnamed still contributes its fan-out, uncollapsed. `CheckQualifiedReducerBody`'s
"must come from the qualifier" rule generalizes the same way — a column must
come from *one of* the named relations, not from a single fixed one, and a
decision variable must be scoped to a relation in that set (or be a query-wide
scalar, as before).

This has one consequence worth stating plainly: naming every relation a query
joins is a no-op. With exactly two relations in the query, `SUM(D, T: expr)`
and unqualified `SUM(expr)` are the same reducer, because the composite key
already *is* the join-result row — there is no third, unnamed relation left to
contribute fan-out for the qualifier to collapse. The two forms only diverge
once a relation is left out of the list while still appearing in the join.

The relation list is order-independent — `FindOrCreateEntityScope` canonicalizes
it (sorted, case-insensitively) before using it as the scope cache key, so
`SUM(D, T: ...)` and `SUM(T, D: ...)` share one scope and one `EntityMapping`
rather than building the identity twice.

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

`GetExpressionType`'s `norm`/`SUM`/`AVG`/`MIN`/`MAX` classification is shared with
`DecideObjectiveBinder` through `DecideBinder::ClassifyReducerCall` — the base class
both subclasses already derive from. The one real difference between a constraint and
an objective reducer stays a parameter rather than getting flattened: constraints pass
`allow_bilinear=true` to the argument validator, objectives don't, since a bilinear
term is accepted in a `SUCH THAT` clause and refused in `MAXIMIZE`/`MINIMIZE`. Each
subclass keeps its own fallthrough for a non-reducer function — the shapes a
constraint's left-hand side and an objective body accept past that point diverge.

### What may be a bound

When one side reduces (`SUM`, `AVG`, `MIN`, `MAX`), the other side must reduce to a
single value too, and `IsAllowedDecisionFreeBoundExpression` decides which shapes
qualify: constants, operators and functions over allowed operands, casts, scalar
subqueries, and data-only reducers with their own `WHEN`. A bare column does not
qualify — it has one value per row, not one per group. Arithmetic composes freely,
`-` included: a bound is evaluated as an expression over the row, so subtraction is
no different from addition. (`-` alone was refused for a while, a leftover from the
parsed-level symbolic layer that used to move terms across the comparison. It also
rejected `-5.0::DOUBLE`, where the minus is the literal's own sign, so a negative
bound could not be written with a cast at all.) Wrapper tags recurse into child 0
only: a `WHEN` predicate or a `PER` key is not a value on that side.

### Subqueries

Both uncorrelated and correlated **scalar** subqueries are supported, and are
delegated to DuckDB's standard `ExpressionBinder::BindExpression`. Non-scalar
subqueries are rejected, as is any subquery referencing a decision variable
(`ExpressionContainsDecideVariable`). Correlation is observed later, in
`plan_select_node.cpp`, at the only point where it is still visible — see
[`../03_logical_plan/done.md`](../03_logical_plan/done.md).

### `<`, `>` and `<>` over a REAL decision

All three are encoded by stepping the bound one integer unit: `< K` becomes `<= K-1`,
and `<> K` becomes the disjunction `<= K-1 OR >= K+1`. Both are exact only when the
compared side lands on integers.

For `<` the step cuts feasible points: `SUM(x) = 4.7` satisfies `< 5` but not `<= 4`.
For `<>` the problem is worse than inexact — `{v : v != K}` over the reals is an **open**
set, and every MILP feasible region is a finite union of closed polyhedra, so `<>` on a
continuous quantity has no correct encoding at all. Excluding a single point from a
continuous range rules out nothing a solver can act on.

The declared type settles that without reading a row, so
`ValidateDecideNoIntegerStepComparisonOnReal` rejects all three here, naming the
variable and quoting the clause. It runs on the parsed tree beside the other DECIDE
validators, and it checks **comparisons in constraint position only** — descending
through conjunctions and through the constraint child of a `WHEN` / `PER` wrapper, the
same way `DecideConstraintsBinder` dispatches. A comparison nested inside an operand is a
boolean value, not a model row: in the misparse `(SUM(x) WHEN w > 1) + 3 <= 10` the
`> 1` is added to `3`, and the resulting type error is the better diagnosis.

Both sides are read, because canonicalization has not run yet and `5 > SUM(x)` is as
likely a spelling as `SUM(x) < 5`. Reading a side is not moving one.

The advice differs by operator because the repair does. `<` has an exact restatement
(`<= K-1`), so the message offers it. `<>` has none, so the message says to declare the
decision `INT` if the quantity is a whole number, or to name the range actually meant.
The `<>` clause is spelled by hand rather than through `ToString()`, which renders
`COMPARE_NOTEQUAL` as `!=` — a clause quoted back to the user has to read the way they
wrote it.

**The rule is stated on the type of the compared quantity, not on every type beneath
it.** `norm(e, 0, M)` counts nonzeros, so its value lands on the integer lattice however
`e` is declared, and the integer step is exact over it. `IsIntegerValuedReducer` stops the
search there, so `norm(x, 0, M) < 3` and `norm(x, 0, M) <> 3` are both **accepted** over a
REAL `x`. Only p=0 qualifies: `norm(e, 1)` sums magnitudes, `norm(e, 2)` is a length and
`norm(e, 'inf')` a maximum, each as continuous as `e`.

> Reversed 2026-08-17. The rule first shipped stated on the declared type of `e`, so that
> what DECIDE accepts would not depend on which linearization a term receives. That
> principle stands — L0 integrality simply is not a linearization choice, it is the
> definition of the reducer, so the earlier rule was type-checking the wrong quantity.
> Settled alongside the `<>` split so both operators answer alike; before it, `<` refused
> `norm(x, 0, M)` over a REAL decision and `<>` accepted it.

### The rest of the compared expression

`ValidateDecideIntegralComparisonOperands` completes the same refusal for every operand
that is not a decision: data columns, literals, and the reducers over them. It runs on the
**bound** tree, immediately after `DecideConstraintsBinder`, because a parsed tree carries
no types yet.

**It is a type judgement, not a value judgement.** `l_quantity` is `DECIMAL(15,2)` in
TPC-H and every row of it holds a whole number, so `SUM(x * l_quantity) < 100` would in
fact step exactly — and it is refused anyway. The alternative, reading the stored values,
makes a query's *validity* depend on the table's contents: the same query is legal today
and illegal tomorrow because someone inserted `0.05`. Whether a query is well-formed
should follow from the query and the schema.

The message therefore names the column, its type, and the cast that fixes it — the cast
is the user stating the assumption the rewrite depends on:

> Comparison '<' is not supported here: column 'l_quantity' has type DECIMAL(15,2), which
> allows fractional values, and stepping the bound is exact only on whole numbers. If
> 'l_quantity' holds whole numbers, cast it (l_quantity::BIGINT); otherwise compare with
> '<='.

`DECIMAL(p, 0)` carries its scale in the type and needs no cast. The rule is "scale > 0,
or a floating type", not "any DECIMAL".

Four things are deliberately **not** refused:

- **The bound `K`.** Only a side referencing a decision is a compared quantity; the other
  side is the bound. A fractional `K` is not an error but a *tautology* — no whole number
  equals 2.5 — so `SUM(x) <> 2.5` is dropped, and an infinite `K` reads the same way.
  Refusing here would turn two well-defined outcomes into errors.
- **A decision-free addend.** `x + 1000003.50 < K` is exactly `x < K - 1000003.50`; a
  fractional offset moves the bound, it does not move the lattice. A *multiplier* is
  different — `0.5 * x` rescales the decision off the lattice — so every factor of a
  product is checked whether or not it carries a decision.
- **`POWER(e, n)` with a whole, non-negative constant `n`**, which returns `DOUBLE` but
  stays on the lattice when `e` does. Its exponent is read through the binder's inserted
  casts.
- **`AVG(e) <> K`**, whose denominator is hoisted to the right-hand side as
  `SUM(e) <> K*n`, leaving an integral left side. The hoist is specific to `<>`;
  `AVG(e) < K` keeps its fractional `1/n` coefficients and is refused.

> Moved here 2026-08-17, from an `InvalidInputException` in the model builder that read
> evaluated coefficient *values*. The two halves of the gate now answer to one rule, in
> one layer, before any data is read, and the model builder's remaining checks are
> `InternalException` invariants.

### `PER`

`BindPerConstraint` requires the `PER` expression to reference a table column —
not a decision variable, not a constant. Whether `PER` is *eligible* on this
constraint is decided after canonicalization, using the canonical aggregate /
per-row classification, not by a parsed aggregate-shape guess. Combined
expression-level `WHEN` + `PER` filters first, then groups.

Once that validation passes, both `BindPerConstraint` and the objective's `PER`
handling assemble their tagged `BoundConjunctionExpression` through the same
`DecideBinder::BindPerWrapper`: bind child 0 (the wrapped constraint or objective)
through the subclass's own dispatch, bind the PER columns through the base
`ExpressionBinder`, tag the result with `func.function_name` for the canonicalizer
to read back as the PER key.

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
| Shared DECIDE expression rules, reducers | `src/planner/expression_binder/decide/decide_binder.cpp` |
| Degree — the one definition, and the gate over it | `src/planner/expression_binder/decide/decide_degree.cpp` |
| `SUCH THAT` binding and `PER` gate | `src/planner/expression_binder/decide/decide_constraints_binder.cpp` |
| Objective binding | `src/planner/expression_binder/decide/decide_objective_binder.cpp` |
| Entity scope struct | `src/include/duckdb/planner/operator/decide/logical_decide.hpp` |
| Scope enum and DECIDE tags | `src/include/duckdb/common/enums/decide.hpp` |
