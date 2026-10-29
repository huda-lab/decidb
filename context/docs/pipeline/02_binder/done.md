# Stage 02 — Binder

Resolves names, scopes and types; recognizes reducers; and decides whether a
DECIDE clause is *semantically valid*. It produces a bound representation and
nothing else — it does not move terms across a comparison, flip a relation, or
choose a solver formulation.

**Key source files**

- `src/planner/expression_binder/decide/decide_declarations_binder.cpp` —
  declarations, their scopes and types, the parsed-tree validators, then the two
  clause binders (`BindDeclarations`; the DECIDE section of `BindSelectNode` in
  `bind_select_node.cpp` only calls it)
- `src/planner/expression_binder/decide/decide_binder.cpp` — shared DECIDE expression
  rules: reducers, the colon form, a reducer's own scope and `by`, key scopes, and where
  `when`, `per` and `by` may stand
- `src/planner/expression_binder/decide/decide_constraints_binder.cpp` — `SUCH THAT`
- `src/planner/expression_binder/decide/decide_objective_binder.cpp` — `MAXIMIZE` / `MINIMIZE`

---

## 1. Variable declarations

Each entry of `statement.decide_variables` arrives as a `PG_AEXPR_OF` comparison
pairing a `ColumnRefExpression` with a type marker string; with a `per`, the name is
wrapped in the parser's `SCOPE_PER_TAG` marker (children: the name, then the key), or
in `SCOPE_PER_ROW_TAG` for `per row`. `SplitDeclaration` reads the name, the `per`
marker and the type. Every declaration's name is checked and collected before any key
resolves, so a key can be told from a decision declared before or after it. The checks:

| Check | Message |
|---|---|
| Name collides with a column of the FROM clause, a `USING` column included | `DECIDE variable '%s' conflicts with an existing column name.` |
| Name declared twice | `Duplicate DECIDE variable name '%s'.` |

A decision is registered under its bare name, and only that name references it.
`RefuseQualifiedDecisionReferences` refuses `D.x`, where `D` is a relation of the FROM
clause with no column `x` and `x` is a decision, over the constraints, the objective,
the select list and `ORDER BY`, before anything else reads them: *"DECIDE: write x, not
D.x; a decision is referenced by its bare name"*, or, in a `when` condition, a `per` key
or a `by` key, *"DECIDE: D.x is a decision; when, per and by may only use data"* (D43).
A qualified name that binds to a column, a struct field or a `USING` column is left to
the SQL binder.

WHERE is planned below the DECIDE operator, where a decision has no value yet, so it
refuses one. After WHERE binds, `RefuseDecisionInWhere` (`bind_select_node.cpp`) looks
for the DECIDE binding in the filter, directly or as a correlated column of a subquery:
*"DECIDE: x is a decision; WHERE may only use data"*. HAVING, QUALIFY, GROUP BY,
ORDER BY and the select list are planned above DECIDE and read its result, so a
decision is a column there.

### Scope

Three scopes, recorded per variable in `variable_scopes` as a `DecideVarScopeInfo`
(`common/enums/decide.hpp`; the enum `DecideVarScope` is `ROW`, `ENTITY`, `SCALAR`):

| Declaration | Scope | Meaning |
|---|---|---|
| `x(TYPE)`, `per row: x(TYPE)` | `Row()` | one decision per result row |
| `per K: x(TYPE)` | `Keyed(scope_idx, K as written)` | one decision per class of `K` |
| `per (): x(TYPE)` | `Global()` | one decision for the whole query |

`Keyed` is an `ENTITY` scope that also records `declared_key`, so a message can name
the decision as it was declared; everything past the binder treats it as `ENTITY`.
`per ()` creates no key scope. The names of the query-wide decisions travel to the two
clause binders (`scalar_variables`), which treat such a decision as one value.

Every key the clauses write is an entry of one key table (`DecideKey`,
`logical_decide.hpp`; [../03_logical_plan/done.md §4](../03_logical_plan/done.md#4-what-logicaldecide-carries)):
the key as first written (`D.depotID, S.customerID`, `D`, `()`) and its elements, each
a bound expression with the name a message prints and a NULL mode (`VALUE` for a column
a relation element stands for, `NO_GROUP` for one written on its own).
`FindOrCreateDecideKey` finds the entry with the same elements as a set, or appends one.
A keyed decision points at an `KeyedScopeInfo`: the key, the decisions it keys
(`scoped_variable_indices`), and `value_link`, which only stage 05 sets, on a helper
decision's scope. One scope exists per key, whichever spelling names it: a declaration's
`per K`, a reducer's own `per K` and the colon form `SUM(D: e)` (§3) all go through
`FindOrCreateKeyScope` or, for the colon form's relation list, `FindOrCreateScope`.

### Keyed declarations

`FindOrCreateKeyScope` (`decide_binder.cpp`) builds the scope of `per K:` from the
parser's marker. Each key element resolves against the query's own bind context —
never an outer query, whose column would be one constant per run of this one:

1. A declared decision, by its name: *"DECIDE: x is a decision; when, per and by may
   only use data"*. A decision named like a column was already refused by the
   conflict check (a `USING` column escapes it, a logged bug), so a name never means
   both.
2. A column, resolved as SQL resolves it (`BindContext::GetBinding(alias, column)`):
   `R.c`, `S.R.c`, `C.S.R.c`, or an unqualified `c` — the `USING` primary relation's
   for a `USING` column (a `FULL OUTER JOIN USING` column has none: *"... write
   S.depotID or D.depotID in the per key"*), else the one relation that has it
   (DuckDB's ambiguity error otherwise).
3. A relation (`R`, `S.R`, `C.S.R`), standing for its stored columns.
4. Otherwise *"DECIDE: depot is neither a column nor a relation of the FROM clause"*.

`rowid` and a generated column are not stored columns: named, they are an error;
inside a relation, they are skipped. Columns are registered through
`TableBinding::GetColumnBinding` for a base table, so the scan keeps them.
`FindOrCreateDecideKey` de-duplicates them and puts them in one order (columns by
binding, then other expressions by their text), so equal keys share one entry and one
grouping however they were written.

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

**Volatile functions are refused** (`RefuseVolatile`, `decide_declarations_binder.cpp`),
over the whole bound constraint tree, the objective and every `by` key: *"DECIDE:
random() is not supported in a constraint"*. The check reads each function's own
stability, since `IsVolatile()` is also true for every expression above a volatile one.
Every later stage reads a clause more than once (the scope checks, the rows, the
bound-side window, direct solve's plan), so one volatile value would select different
rows each time; refusing it here is what lets those stages evaluate a clause freely.
A value computed once per row in a subquery or CTE before DECIDE is data and stays
legal. Functions constant within one query (`now()`, `txid_current()`) are allowed.

`ValidateDecideConstraintDegree` and `ValidateDecideObjectiveDegree` run in
`decide_declarations_binder.cpp` immediately after the constraint and objective binders,
and they are **total**: the constraint validator descends conjunctions and the
`when` and `per` wrappers to every comparison that becomes a model row, and checks
both sides. That totality is the point: a check scoped to reducer arguments would
miss a bare per-row constraint, so `SUM(POWER(x*y,2)) <= 10` and `POWER(x*y,2) <= 5`
would be refused by different layers, in different words, depending on spelling.

**Why the judgement stays here rather than moving to stage 05**, which is the
only stage that can see the tree after its rewrites have run: validity must
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
propagation `ExpressionBinder::Bind` does at its own entry point, so without help the
bound tree would reach the post-binding validators with no location.
`DecideBinder::PreserveQueryLocation` stamps it in each `BindExpression`
override — filling only an empty location, so an inner node keeps its own span
rather than being widened to its parent's. The degree gate needs this to point a
caret, and the integrality gate's two refusals (§4) gained one as a side effect;
they had been passing no node to `BinderException` because there was nothing
useful to pass.

---

## 3. Reducers

`IsDecideAggregateName` recognizes `SUM`, `AVG`, `MIN`, `MAX` as DECIDE reducers.
`DecideBinder::BindAggregate` binds them; the optimizer, not the binder, decides
their formulation. `norm(e, p)` binds as an inert `SUM` marker tagged with its order
(`NORM_MARKER_TAG_PREFIX`), which stage 05 lowers. `COUNT` binds as data only:
`COUNT(x)` over a decision is refused as degenerate.

### A reducer's own `when` and `per`

`DecideBinder::BindScopedReducer` peels `[when c] [per K]:` off a reducer's argument
and binds the reducer over the body. The condition binds as data, cast to `BOOLEAN`,
onto `BoundAggregateExpression::filter`, which physical extraction copies onto each
term the reducer produces. A `per K` becomes a `__qualified_by_<i>__` tag on the exact
key scope of `K` (`FindOrCreateKeyScope`; an empty key for `per ()`), whose classes the
reducer counts once. `per row` adds nothing. A `by` after the reducer is
`BindReducerBy` (§4a).

### Relation-qualified reducers

`DecideBinder::BindQualifiedReducer` binds the colon form `SUM(D: expr)` as
`sum(per D: expr)` (../../language/guide.md §5.1): each listed relation must be a relation
of the FROM clause, its stored columns form the key (`AddRelationColumns`), and `FindOrCreateScope` — shared with `FindOrCreateKeyScope` —
finds or creates the key's scope, so `SUM(D: e)` and `sum(per D: e)` share one
scope and one tag (`__qualified_by_<i>__`, appended to the aggregate's alias). `COLON_FORM_TAG` keeps the
spelling for the echo (D38). Nothing is checked against the body at bind time: that `D`
determines it — every column, per-row and keyed decision in it — is the DETERMINES
check of stage 04, on the data, as for `per` (*"DECIDE: per D does not determine the
decision y (in sum(D: ...))"*). Refused at bind time: a qualifier that is not a relation
name, a relation not in the FROM clause, a qualifier on anything but `SUM`, `AVG`,
`MIN`, `MAX` or `norm`, and on the decision side a body of query-wide decisions alone
(`ClassifyReducerCall`, as for any reducer). `norm` binds as a `SUM` marker, so `norm(D: e, p)`
carries the tag like `SUM(D: e)`; stage 04's definition of orders 1, 2 and `'inf'` and stage
05's L0 lowering keep the alias, so every order reads each row of `D` once.

A reducer may name more than one relation, `SUM(D, T: expr)`. The key is every named
relation's stored columns, so a row is a duplicate only when it repeats on **every**
named relation's key at once. Concretely: naming a relation removes the fan-out it
would otherwise contribute; a relation left unnamed still contributes its fan-out,
uncollapsed.

This has one consequence worth stating plainly: naming every relation a query
joins is a no-op. With exactly two relations in the query, `SUM(D, T: expr)`
and unqualified `SUM(expr)` are the same reducer, because the composite key
already *is* the join-result row — there is no third, unnamed relation left to
contribute fan-out for the qualifier to collapse. The two forms only diverge
once a relation is left out of the list while still appearing in the join.

The key is a set of columns, sorted by binding, so `SUM(D, T: ...)`, `SUM(T, D: ...)`
and `sum(per D, T: ...)` share one scope and one `EntityMapping`.

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
`BindConjunction`, `BindScopedConstraint` (§4a).

Constraints are joined with `AND`. `BindConjunction` refuses any other conjunction in a
constraint position (`x <= cap OR x <= 1`, also under a scope): every later walk reads a
conjunction as `AND`, and no layer builds the indicator an `OR` needs. Only a constraint
list reaches `BindConjunction`: a conjunction inside an operand is data or refused (Data
operands, below), and an `OR` in a `when` condition, a reducer's `when` or a subquery is
data.

`GetExpressionType`'s `norm`/`SUM`/`AVG`/`MIN`/`MAX` classification is shared with
`DecideObjectiveBinder` through `DecideBinder::ClassifyReducerCall` — the base class
both subclasses already derive from. The one real difference between a constraint and
an objective reducer stays a parameter rather than getting flattened: constraints pass
`allow_bilinear=true` to the argument validator, objectives don't, since a bilinear
term is accepted in a `SUCH THAT` clause and refused in `MAXIMIZE`/`MINIMIZE`. Each
subclass keeps its own fallthrough for a non-reducer function — the shapes a
constraint's left-hand side and an objective body accept past that point diverge.

### What may be a bound

When one side reduces (`SUM`, `AVG`, `MIN`, `MAX`), the side that carries no decision
is the bound, and `IsAllowedDecisionFreeBoundExpression` decides which shapes it may
take: constants, data columns, operators and functions over allowed operands, casts,
scalar subqueries, and data-only reducers with their own `when`, `per` and `by`. A data
column is a bound like any other: a clause with no `per` is generated once per kept
row, so the tightest row's value binds; under `per K:` the key must determine it, which
is a run-time check (stage 04, "Scope checks"). Arithmetic composes freely, `-`
included: a bound is evaluated as an expression over the row, so subtraction is no
different from addition, and `-5.0::DOUBLE` is a negative bound like any other. A colon-form or `by` marker recurses
into child 0 only: its relations or key are not values on that side. A data-only
reducer never shares a scaled term with a decision (S10): `ValidateDecideNoNonLinearScalar`
refuses `x * avg(cap)`, `sum(x) / sum(cap)` and `(sum(x) + sum(cap)) / 2` on the parsed
tree.

### Data operands

Below a clause's top, an operand that reads no decision is data, and DuckDB binds it by
its own rules (`DecideBinder::IsDataOperand`). The shape does not matter: a comparison
read as 0/1 (`x <= 4 * CAST(cap > 2 AS INT)`), a lambda and its parameters
(`sum(x * list_reduce([cap, b], (a, z) -> a + z))`), a prepared statement's `$1`. A
function, a subquery and a CASE keep their DECIDE handling: a function may be a reducer
or a scope marker, a subquery has the checks below, and a CASE is refused wherever it
stands (`DecideCaseUnsupportedMessage`).

The rule holds in four places, one per walk that reads an operand:

- `DecideConstraintsBinder` and `DecideObjectiveBinder`, before their dispatch.
- The reducer-argument validator (`ValidateSumArgument`): a shape it does not list is a
  per-row coefficient when it reads no decision, as a data-only operator is.
- The bound check (`IsAllowedDecisionFreeBoundExpression`): a parameter is one value, and
  a comparison, conjunction, `BETWEEN` or lambda is allowed when every part is.

A comparison, `BETWEEN` or conjunction that reads a decision, below the top, is neither a
constraint nor data. `x <= (x > 1 AND cap > 1)` has a meaning in SQL (DuckDB casts the
boolean to 0/1), but modelling it needs an indicator no layer builds, so it is refused
with the clause quoted: *"SUCH THAT does not support '((x > 1) AND (cap > 1))'"*. The
same shape inside a reducer gets *"SUM expression does not support '(x BETWEEN 1 AND
2)'"*, and in the objective *"MAXIMIZE does not support '(x > 1)'"*. Written with an
explicit cast, it meets the cast rule first.

A table macro's parameter reaches a clause because `ParsedExpressionIterator` visits a
DECIDE's `SUCH THAT` and objective, which macro substitution walks (`ReplaceMacroParameters`).
The CTE reference count and the prepared-statement verifier walk them too. Declarations
are not visited: they hold names, types and key columns, never a value.

### Subqueries

Both uncorrelated and correlated **scalar** subqueries are supported, and are
delegated to DuckDB's standard `ExpressionBinder::BindExpression`. Non-scalar
subqueries are rejected, as is any subquery referencing a decision variable
(`ExpressionContainsDecideVariable`). The DECIDE walks over a subquery's body read it as
ordinary SQL and pass `decide_clauses = false` to `ParsedExpressionIterator`: a DECIDE
nested in it keeps its clauses to itself, and its own binder checks them, so its `per ():`
is not taken for a scope written inside a subquery.

A DECIDE inside a subquery reports its own refusal. When an expression fails to bind in a
subquery, DuckDB retries it in each outer query, in case it refers to one, and reports the
retry's error, so an inner *"SUCH THAT: OR is not supported"* would reach the user as the
outer *"Referenced column "y" not found"*. Only a missing name can be an outer reference,
so `DecideBinder::ThrowClauseError` throws any other error at the clause's root, before
the retry; a column or table not found (`COLUMN_NOT_FOUND`, `TABLE_NOT_FOUND`) goes on to
it. `BindContext` marks a table not found with `TABLE_NOT_FOUND` for this.

A DECIDE cannot read a column of an outer query, in any clause or in its `WHERE`: it solves
one problem, and re-solving it per outer row is not built. At the end of `BindSelectNode`,
a DECIDE query whose binder holds a correlated column is refused, naming it: *"DECIDE: 'k'
refers to an outer query, which a DECIDE cannot read; to solve per group, join that table
into this query's FROM and write the clause with per"*. A subquery inside a clause that is
correlated to the DECIDE's own rows keeps that correlation on its own binder, so it is not
caught. A clause reaches the outer column through the retry above (a column or a table not
found); when the column sits inside a reducer, SQL binds the whole aggregate in the outer
query, and the message quotes the reducer.

Correlation is observed later, in
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
through conjunctions and through the constraint child of a `when` / `per` scope, the
same way `DecideConstraintsBinder` dispatches. A comparison nested inside an operand is
a boolean value, not a model row, and is left to the type checks.

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

What DECIDE accepts does not depend on which linearization a term receives. L0
integrality is not a linearization choice: it is the definition of the reducer, so the
type that matters is the count's, not `e`'s. `<` and `<>` answer alike.

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
  product is checked whether or not it carries a decision. Where the side is not linear
  in the addend it counts toward the value (§4a).
- **`POWER(e, n)` with a whole, non-negative constant `n`**, which returns `DOUBLE` but
  stays on the lattice when `e` does. Its exponent is read through the binder's inserted
  casts.
- **`AVG(e) <> K`**, whose denominator is hoisted to the right-hand side as
  `SUM(e) <> K*n`, leaving an integral left side. The hoist is specific to `<>`;
  `AVG(e) < K` keeps its fractional `1/n` coefficients and is refused.

Both halves of the gate answer to one rule, in one layer, before any data is read. The
model builder's remaining checks are `InternalException` invariants.

### Refusals quote the clause

A constraint shape the binder does not read is refused by quoting it as written,
through one helper (`UnsupportedConstraint`): *"SUCH THAT does not support '(x IS
NULL)'"*. A reducer body it does not read gets *"SUM expression does not support
'(x BETWEEN 1 AND 2)'"*. A function on either side is named as the user wrote it
(`DecideWrittenFunctionName`): `count(*)`, not DuckDB's internal `count_star`. A
reducer whose body holds no decision (`sum(1)`, `sum(cap)`) gets *"SUM expression must
reference at least one DECIDE variable"*; only a body that holds a query-wide decision
is told to drop the reducer.

---

## 4a. Scopes — `when`, `per`, `by`

Every clause follows the scope rules: a clause with no scope written is `per row:`, and
a reducer with no `by` is `by ()` (../../language/guide.md §1.1).
`ValidateDecideScopeSpellings` (`decide_binder.cpp`) runs on the parsed constraints
(one `AND` operand at a time) and on the objective, before either binds:

- a scope or `by` inside an ordinary SQL subquery written in the clause, a `when`
  condition or a key: *"DECIDE: 'PER key:' is not supported yet"* (or `'PER ():'`,
  `'PER ROW:'`, `'WHEN condition:'`, `'BY (key)'`, whichever was written);
- a scope inside a call other than `SUM`/`AVG`/`MIN`/`MAX`: *"DECIDE: when and per
  inside a call need SUM, AVG, MIN or MAX; write the scope in front of the constraint
  or inside a reducer"* (D15); a `by` after anything but those four, `norm` or `count`:
  *"DECIDE: by (...) must directly follow SUM, AVG, MIN, MAX, norm or count"* (D34,
  D35). After a special form (`COALESCE`, `NULLIF`, `CAST`) or a parenthesized
  expression a `by` is a syntax error, as the grammar admits it only after a function
  call;
- a decision in a `when`, a key or a `by`: *"DECIDE: x is a decision; when, per and by
  may only use data"*; a window function there: *"DECIDE: a window function such as
  'row_number() OVER ()' is not supported in when, per or by"*; a reducer there (`when
  sum(cap) > 1:`, `by (max(cap))`): *"DECIDE: a reducer such as 'sum(cap)' is not
  supported in when, per or by"*. Each reads rows the scope has not chosen yet, so
  `ValidateScopeData` refuses them with a caret, in that order, before execution sees the
  query. A `per` key cannot hold one: the grammar takes only columns and relations there;
- a reducer inside a reducer: *"DECIDE: an aggregate inside an aggregate is not supported
  yet"*, except the objective's two-level form (`IsTwoLevelObjective`), and except a
  reducer over data alone under a `*` or `/` that holds a decision (`sum(avg(cap) * x)`):
  that is a coefficient, refused by `ValidateDecideNoNonLinearScalar` with its remedy.

| Written | Bound |
|---|---|
| `when c: C` | a WHEN wrapper (`WHEN_FILTER_TAG`) `[C, c]` |
| `per K: C` | a `PER_CLASS_TAG` wrapper `[C]` tagged `__per_key_<i>__` with K's entry of the key table (no tag for `per ()`, which has no entry), its key as written a source fragment on the alias |
| `when c per K: C` | `PER_CLASS{WHEN{C, c}}`, tagged with K |
| `per row: C`, `agg(per row: e)`, `per row: x(TYPE)` | nothing: the default written out (`SCOPE_PER_ROW_TAG`, peeled) |
| `agg(...) by (G)` | the aggregate tagged `__group_key_<i>__`, naming `BoundSelectNode::decide_keys[i]`, whose `written_as_by` is set; `by ()` adds nothing |
| `agg(when c: e)` | the aggregate's `filter` (§3) |
| `agg(per K': e)`, `SUM(D: e)` | a `__qualified_by_<i>__` tag on an exact key scope; the colon form adds `COLON_FORM_TAG` (§3) |
| `MAXIMIZE when c: E` | a WHEN wrapper over the objective |
| `outer(per k: inner(e) by (k))` | `PER{outer(inner(e))}` (`TWO_LEVEL_OBJECTIVE_TAG`), tagged with k's entry, its key as written a source fragment; a `when` in front sits inside it, `PER{WHEN{outer(inner(e)), c}}` |

- `BindScopedConstraint` binds the body, the condition as data, the key through
  `BindKey` (a declaration key's resolution into the key table; a column element keeps
  its written name for messages). A `BETWEEN` binds to two comparisons, each under the whole scope;
  the second binds the condition again from its parsed text, since a bound subquery
  cannot be copied.
- The key's text becomes a source fragment while binding, not on the parsed marker:
  `ExpressionBinder::Bind` copies a parsed node's alias onto the node it binds to, which
  would replace the wrapper's tag.
- `BindReducerBy`: a relation element stands for its stored columns (a column of that
  name wins); any other element binds as data. Equal key sets share one entry of the key
  table, with every other key of those elements: equal as bound expressions, or, for a key
  holding a subquery, written alike — a bound subquery equals nothing, not even itself.
- `BindScopedReducer` peels the inner `per`/`when` (§3); `SUM(D: e)` is `sum(per D: e)`
  (`BindQualifiedReducer`, §3) and may take a `by`.
- `BindScopedObjective`: `per ()` adds nothing; a `when` wraps the body, inside the
  two-level form's `TWO_LEVEL_OBJECTIVE_TAG` wrapper. `BindTwoLevelObjective` refuses a `when` inside either
  reducer, a `per` inside the inner one, a `by` key that is not the `per` key, and an
  outer `avg` (*"DECIDE: avg over the classes of avg(per g: sum(...) by (g)) is not
  supported yet; write sum, min or max over them"*). A decision outside the objective's
  reducers binds: a query-wide one is one column, and any other must have one instance
  over the kept rows, a run-time check. The objective's casts and subqueries are
  fragment-tagged like a constraint's.
- A strict comparison over a fractional operand names a reducer as written, scopes and
  `by` included (*"'avg(ship) by (D.region)' has type DOUBLE ..."*). A decision-free
  addend belongs to the bound where the side is linear in it. A `MIN` or `MAX` that is a term
  of the side hands its constant addends to the bound in stage 04 (`max(x + 0.5) > 3` is
  `max(x) > 2.5`), so only the terms that stay count; `SplitExtremumBody` is the one split both
  stages read. A column addend stays (`max(x + w) > 3` is refused). `DecideSideIsWholeNumbered`'s
  `addends_count` makes an addend part of the value under a power, in a product of decisions
  and in a norm of order 1, 2 or `'inf'` (`POWER(x + 0.25, 2) < 11`, `POWER(max(x + 0.5), 2) > 6`,
  `(x + 0.5) * y < 5`, `norm(x + 0.5, 'inf') > 3`).
  `DecideValueIsWholeNumbered` exports the same test with every addend counted, for the
  value rewrite's helper decisions (stage 05).
- A scalar function over a decision that binds to a NULL constant (`x + NULL`: DuckDB
  folds it) is refused in `DecideBinder::BindFunction`, where the SQL is still at hand:
  *"DECIDE: '(x + NULL)' is NULL whatever the decisions are; replace the NULL with a
  value, as in COALESCE(column, 0)"*. The decision would be gone from the bound tree.
- A `SUM`, `AVG`, `MIN`, `MAX` or `norm` with no argument is refused: *"SUM needs one
  argument, found 'sum()'"*, *"NORM needs an expression and an order, as in norm(e, 1),
  found 'norm()'"*.

---

## 5. Objective

`DecideObjectiveBinder` binds `MAXIMIZE` / `MINIMIZE` bodies through the same
`DecideBinder` rules. It knows the sense (`decide_sense`) because a few checks
depend on it. A scope in front (`BindScopedObjective`) and the two-level form
(`BindTwoLevelObjective`) are bound as §4a describes. It performs no algebraic
rewriting: the objective's shape — its additive spine, its reducer scales, its
constant offset — is decided once by `DecideCanonicalizer::CanonicalizeObjective` on
the bound tree, alongside constraints.

A body it cannot read is refused in SQL terms, with the sense the user wrote: a body
with no decision gets *"MAXIMIZE 3 has no decision to optimize"*, any other *"MAXIMIZE
does not support '(x > 1)'"*. A decision-free operand below the top is data (§4, Data
operands).

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
| Declarations, scopes, types, the parsed-tree validators (`BindDeclarations`, `RefuseQualifiedDecisionReferences`) | `src/planner/expression_binder/decide/decide_declarations_binder.cpp` |
| Shared DECIDE expression rules (`IsDataOperand`), reducers, key scopes (`FindOrCreateKeyScope`, `FindOrCreateScope`, `BindKey`), where scopes may stand (`ValidateDecideScopeSpellings`) | `src/planner/expression_binder/decide/decide_binder.cpp` |
| Degree — the one definition, and the gate over it | `src/planner/expression_binder/decide/decide_degree.cpp` |
| `SUCH THAT` binding and a constraint's scope (`BindScopedConstraint`) | `src/planner/expression_binder/decide/decide_constraints_binder.cpp` |
| Objective binding, its scope and the two-level form | `src/planner/expression_binder/decide/decide_objective_binder.cpp` |
| Refusing a DECIDE that reads an outer query | `src/planner/binder/query_node/bind_select_node.cpp` |
| Which parts of a query a walk visits (`decide_clauses`) | `src/parser/parsed_expression_iterator.cpp` |
| Key scope struct | `src/include/duckdb/planner/operator/decide/logical_decide.hpp` |
| Scope enum and DECIDE tags | `src/include/duckdb/common/enums/decide.hpp` |
