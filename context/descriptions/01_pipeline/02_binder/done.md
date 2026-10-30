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
| Name collides with a real column | `DECIDE variable '%s' conflicts with an existing column name.` |
| Name declared twice | `Duplicate DECIDE variable name '%s'.` |

A decision is registered under its bare name, and only that name references it.
`RefuseQualifiedDecisionReferences` refuses `D.x`, where `D` is a relation of the FROM
clause with no column `x` and `x` is a decision, over the constraints, the objective,
the select list and `ORDER BY`, before anything else reads them: *"DECIDE: write x, not
D.x; a decision is referenced by its bare name"*, or, in a `when` condition, a `per` key
or a `by` key, *"DECIDE: D.x is a decision; when, per and by may only use data"* (D43).
A qualified name that binds to a column, a struct field or a `USING` column is left to
the SQL binder.

### Scope

Three scopes, recorded per variable in `variable_scopes` as a `DecideVarScopeInfo`
(`common/enums/decide.hpp`; the enum `DecideVarScope` is `ROW`, `ENTITY`, `SCALAR`):

| Declaration | Scope | Meaning |
|---|---|---|
| `x(TYPE)`, `per row: x(TYPE)` | `Row()` | one decision per result row |
| `per K: x(TYPE)` | `Keyed(scope_idx, K as written)` | one decision per class of `K` |
| `per (): x(TYPE)` | `Scalar()` | one decision for the whole query |

`Keyed` is an `ENTITY` scope that also records `declared_key`, so a message can name
the decision as it was declared; everything past the binder treats it as `ENTITY`.
`per ()` creates no key scope. The names of the query-wide decisions travel to the two
clause binders (`scalar_variables`), which treat such a decision as one value.

A keyed decision points at an `EntityScopeInfo` (`logical_decide.hpp`): `table_alias`
(the key as written: `D.depotID, S.customerID`, `D`, `()`), `source_table_indices`,
`entity_key_bindings`, `entity_key_column_types`, `scoped_variable_indices`,
`key_names` (each key column as a message names it) and `value_link`, which only stage
05 sets, on a helper decision's scope. `entity_key_physical_indices` is filled in later,
at physical-plan creation. One scope exists per set of key columns, whichever spelling
names it: a declaration's `per K`, a reducer's own `per K` and the colon form
`SUM(D: e)` (§3) all go through `FindOrCreateExactScope`.

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
`TableBinding::GetColumnBinding` for a base table, so the scan keeps them, then sorted
and de-duplicated by binding (`SortedKey`). A scope with the same bindings is reused,
so equal keys share one grouping however they were written.

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
`decide_declarations_binder.cpp` immediately after the constraint and objective binders,
and they are **total**: the constraint validator descends conjunctions and the
`when` and `per` wrappers to every comparison that becomes a model row, and checks
both sides. That totality is the point. The rule previously lived inside
`ValidateSumArgumentInternal`, whose name scoped it to reducer arguments, so
nothing ever applied it to a bare per-row constraint — `SUM(POWER(x*y,2)) <= 10`
was a `Binder Error` while the identical `POWER(x*y,2) <= 5` bound cleanly and
was refused at plan time by term extraction, in extractor vocabulary and with no
source location. Which layer refused depended on how the user spelled it.

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
`sum(per D: expr)` (syntax_reference.md §5.1): each listed relation must be a relation
of the FROM clause, its stored columns form the key (`AddRelationColumns`,
`SortedKey`), and `FindOrCreateExactScope` — shared with `FindOrCreateKeyScope` —
finds or creates the exact key scope, so `SUM(D: e)` and `sum(per D: e)` share one
scope and one tag (`__qualified_by_<i>__`, appended to the aggregate's alias). `COLON_FORM_TAG` keeps the
spelling for the echo (D38). Nothing is checked against the body at bind time: a column,
per-row or keyed decision in it that `D` does not fix makes each tuple one value per
variant (D65), which stage 04's signature records and execution reads, as for `per`.
Refused at bind time: a qualifier that is not a relation
name, a relation not in the FROM clause, a qualifier on anything but `SUM`, `AVG`,
`MIN` or `MAX` (on `norm` by the scope validator: *"DECIDE: norm(D: ...) is not
supported yet; ..."*), and on the decision side a body of query-wide decisions alone
(`ClassifyReducerCall`, as for any reducer).

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
conjunction as `AND`, and no layer builds the indicator an `OR` needs. A conjunction's
children keep its position, so one inside an operand (`CAST((a AND (b OR c)) AS INT)`) is
data, as is an `OR` in a `when` condition, a reducer's `when` or a subquery.

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
row, so the tightest row's value binds; under `per K:` each value it takes in a class is
a variant of its own (stage 04, "Variant signatures"), which binds the same way. Arithmetic composes freely, `-`
included: a bound is evaluated as an expression over the row, so subtraction is no
different from addition. (`-` alone was refused for a while, a leftover from the
parsed-level symbolic layer that used to move terms across the comparison. It also
rejected `-5.0::DOUBLE`, where the minus is the literal's own sign, so a negative
bound could not be written with a cast at all.) A colon-form or `by` marker recurses
into child 0 only: its relations or key are not values on that side. A data-only
reducer never shares a scaled term with a decision (S10): `ValidateDecideNoNonLinearScalar`
refuses `x * avg(cap)`, `sum(x) / sum(cap)` and `(sum(x) + sum(cap)) / 2` on the parsed
tree.

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
  product is checked whether or not it carries a decision. Where the side is not linear
  in the addend it counts toward the value (§4a).
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

---

## 4a. Scopes — `when`, `per`, `by`

Every clause follows the scope rules: a clause with no scope written is `per row:`, and
a reducer with no `by` is `by ()` (syntax_reference.md §1.1).
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
  'row_number() OVER ()' is not supported in when, per or by"*. A reducer there is not
  refused here: execution refuses it (*"DECIDE: an aggregate cannot be used here.
  ..."*, `todo.md`);
- `norm(D: e, p)`: *"DECIDE: norm(D: ...) is not supported yet; a relation qualifier
  takes SUM, AVG, MIN or MAX"* (bound, it would read `SUM(D: e)`);
- a reducer inside a reducer binds, to any depth, in a constraint and in the objective
  (`syntax_reference.md` §5.5; `ValidateSumArgument` holds a nested reducer's body to a reducer's rule), with
  two refusals (D74): a `norm` inside a reducer (*"DECIDE: norm inside a reducer is not
  supported yet"*), and a reducer over data alone added inside one over decisions
  (*"DECIDE: max(...) over data alone inside a reducer over decisions is not supported
  yet; compute it in a subquery, or reduce it outside"*). One under a `*` or `/` that
  holds a decision (`sum(avg(cap) * x)`) is a coefficient, refused by
  `ValidateDecideNoNonLinearScalar` with its remedy. A nest over data alone binds.
  Refused too, as each is rewritten by stage 05 before the value rewrite reads the path
  rule, so the inner reducer would lose the enclosing `when` and `by` (they were answered
  wrongly): a reducer inside `norm` (*"DECIDE: a reducer inside norm is not supported
  yet"*); inside a reducer, a reducer times a decision (*"... multiplies a reducer by a
  decision; only data can scale a reducer"*, the rule the canonicalizer states at the
  root), and `abs` over a reducer where an enclosing reducer has its own `when` or `per`,
  or a `by` (`ScopePlacement::path_keeps_rows`; over a trivial path the rewrite reads the
  same rows and `sum(abs(x - avg(x)))` answers). `count` is a reducer for the "over data
  alone" refusal as the others are (`TakesReducerBy`).

| Written | Bound |
|---|---|
| `when c: C` | a WHEN wrapper (`WHEN_CONSTRAINT_TAG`) `[C, c]` |
| `per K: C` | a `PER_CLASS_TAG` wrapper `[C, key columns...]`, none for `per ()`, its key as written a source fragment on the alias |
| `when c per K: C` | `PER_CLASS{WHEN{C, c}, K}` |
| `per row: C`, `agg(per row: e)`, `per row: x(TYPE)` | nothing: the default written out (`SCOPE_PER_ROW_TAG`, peeled) |
| `agg(...) by (G)` | the aggregate tagged `__group_key_<i>__`, naming `BoundSelectNode::decide_group_keys[i]`; `by ()` adds nothing |
| `agg(when c: e)` | the aggregate's `filter` (§3) |
| `agg(per K': e)`, `SUM(D: e)` | a `__qualified_by_<i>__` tag on an exact key scope; the colon form adds `COLON_FORM_TAG` (§3) |
| `MAXIMIZE when c: E` | a WHEN wrapper over the objective |
| `outer(per k: inner(e) by (k))` | the aggregates nested as written: `outer` tagged `__qualified_by_<i>__` on k's scope, `inner` `__group_key_<j>__` (D75) |
| `at(F: e) over (...)`, `agg(from F1 to F2: e) over (...)` | `first(e)` or `agg(e)` tagged `__frame_<i>__`, naming `BoundSelectNode::decide_frames[i]` (§4a, "Frame selectors") |

- `BindScopedConstraint` binds the body, the condition as data, the key through
  `BindKeyColumns` (a declaration key's resolution; a column element keeps its written
  name for messages). A `BETWEEN` binds to two comparisons, each under the whole scope;
  the second binds the condition again from its parsed text, since a bound subquery
  cannot be copied.
- The key's text becomes a source fragment while binding, not on the parsed marker:
  `ExpressionBinder::Bind` copies a parsed node's alias onto the node it binds to, which
  would replace the wrapper's tag.
- `BindReducerBy`: a relation element stands for its stored columns (a column of that
  name wins); any other element binds as data. Equal key sets share one
  `DecideGroupKey` (expressions, text as written, each element's name): equal as bound
  expressions, or written alike — a bound subquery equals nothing, not even itself.
- `BindScopedReducer` peels the inner `per`/`when` (§3); `SUM(D: e)` is `sum(per D: e)`
  (`BindQualifiedReducer`, §3) and may take a `by`.
- `BindScopedObjective`: `per ()` adds nothing; a `when` wraps the body. A nested
  objective binds like a constraint's body (D75). A decision outside the objective's
  reducers binds: a query-wide one is one column, and any other must have one instance
  over the kept rows, a run-time check. The objective's casts and subqueries are
  fragment-tagged like a constraint's.
- A strict comparison over a fractional operand names a reducer as written, scopes and
  `by` included (*"'avg(ship) by (D.region)' has type DOUBLE ..."*). A decision-free
  addend belongs to the bound where the side is linear in it; `DecideSideIsWholeNumbered`'s
  `addends_count` makes it part of the value inside a `MIN` or `MAX` (stage 06 steps
  the bound on the extremum, so `max(x + 0.5) > 3` is refused), under a power, in a
  product of decisions and in a norm of order 1, 2 or `'inf'`
  (`POWER(x + 0.25, 2) < 11`, `(x + 0.5) * y < 5`, `norm(x + 0.5, 'inf') > 3`).
  `DecideValueIsWholeNumbered` exports the same test with every addend counted, for the
  value rewrite's helper decisions (stage 05).
- A scalar function over a decision that binds to a NULL constant (`x + NULL`: DuckDB
  folds it) is refused in `DecideBinder::BindFunction`, where the SQL is still at hand:
  *"DECIDE: '(x + NULL)' is NULL whatever the decisions are; replace the NULL with a
  value, as in COALESCE(column, 0)"*. The decision would be gone from the bound tree.
- A `SUM`, `AVG`, `MIN`, `MAX` or `norm` with no argument is refused: *"SUM needs one
  argument, found 'sum()'"*, *"NORM needs an expression and an order, as in norm(e, 1),
  found 'norm()'"*.

### Frame selectors

A frame selector (syntax_reference.md §5.6) is checked on the parsed tree with the
scopes, then bound like a reducer whose rows are the ones the frame selects.

- **Where it may stand** (`ValidateDecideScopeSpellings`; D74). `ValidateScopeData` — the
  rule of a `when`, a key and a `by` — refuses a frame there: *"DECIDE: a frame selector
  is not allowed in when, per, by, within or an order key"*. `ValidateFrame` holds the
  frame's order key and `within` key to data (*"DECIDE: x is a decision; a frame's order
  key and within may only use data"*; a frame, a window or a reducer there is refused
  too) and its payload and fallback to one row, linear in the decisions (*"DECIDE: '(x *
  x)' squares or multiplies decisions inside at(...), which is not supported yet"*;
  `FindNonlinearDecisionUse`: any call over a decision but `+`, `-`, `*` and `/`, or a
  decision divisor, *"DECIDE: 'abs((x - 3))' inside at(...) is not linear in the
  decisions, which is not supported yet; write it outside the frame"* — a rewrite would
  lower it into rows standing at the anchor, while the frame reads its payload at
  another row) (*"DECIDE: a reducer inside at(...) is not supported; reduce outside the
  frame"*, *"DECIDE: a frame selector inside at(...) is not supported"*; a range names
  itself `sum(from ...)`). A fallback that reads a decision beside a payload that reads
  none is refused (*"DECIDE: in at(previous else s: t.demand) over (t.period), else s
  reads a decision while the payload reads none, which is not supported yet"*): the
  marker's children are the payload's, so no layer below would see the decision. `ValidateFrameProduct` refuses a
  `*` or `/` with a frame in one factor and a decision in another, and a square of a frame
  over decisions (`power`, `pow`, `**` or `^` with an exponent other than 1): *"DECIDE:
  '(x * at(previous: t.demand) over (t.period))' multiplies a decision by a value from
  another row, which is not supported yet"*. A data factor on a frame over decisions
  (`t.price * at(previous: x) over (...)`) is a linear term and binds.
- **Its place in a clause.** A frame over a decision is a decision side
  (`DecideExpression::VARIABLE` in `GetExpressionType`: one value per anchor row); one
  over data is a bound (`IsAllowedDecisionFreeBoundExpression`). Alone, it is no
  constraint, as a bare decision is none. A frame that reaches a binder of plain SQL (an
  outer query re-binding a correlated DECIDE subquery) is *"DECIDE: a frame selector is
  not supported here: ..."* (`ExpressionBinder::BindExpression`). Inside a reducer,
  `ValidateSumArgumentInternal` holds its payload and fallback to the reducer's rule. In
  the objective a frame over decisions is an objective on its own, as a decision is.
- **The bound form** (`DecideBinder::BindDecideFrame`). The payload binds under the
  aggregate that reads it at the selected rows: a range's reducer (`sum`, `avg`, `min`,
  `max`, or `count` over data, through `BindAggregate`; any other name is *"DECIDE: a
  range frame reduces with sum, avg, min, max or count, found abs"*), and `first` for a
  point frame, which reads one row. The aggregate is tagged `__frame_<i>__`, naming
  `BoundSelectNode::decide_frames[i]` (`DecideFrameSpec`, `../03_logical_plan/done.md`),
  which holds the rest: the positions, `every`, `all`, the order key bound as data, the
  `within` key resolved as a `per` key is (`ResolveKey`: columns, and relations standing
  for their stored columns), and the fallback. The fallback binds like the payload, as a
  number of any numeric or boolean type (*"DECIDE: in at(previous else 'none': x) over
  (t.period), else 'none' is not a number"*), and payload and fallback are cast to their
  common type: a point's is the marker's, so `else 0.5` over a whole-numbered payload is
  0.5 there, and an expression around the frame is bound for a type that holds both; a
  range's reducer is bound again over it (`BindFrameAggregate`), so `sum(from previous to
  next else 0.5: t.n)` sums decimals, and the `else` takes the type the bound reducer
  reads, which its overload may widen the payload to (`sum` of a FLOAT reads a DOUBLE).
  A frame's value is whole (`DecideSideIsWholeNumbered`) only when that shared type is,
  every addend and the `else` part of it, and an AVG range never is: the binder's
  strict-comparison check and the range's helper type (stage 05) read that one test, so
  `sum(from previous to next else 0.5: x) < 3` is refused as fractional. A range's
  reducer name is recorded (`DecideFrameSpec::reducer`), and an AVG range is no AVG the
  `<>` hoist may multiply through (`IsBoundAvgAggregate`): it is fractional,
  *"Comparison '<>' is not supported here: 'avg(from first to last: x) over (t.period)'
  has type DOUBLE and can take fractional values ..."*. A
  fallback that is NULL before the query runs is no fallback (D80; the transformer drops
  a written `else NULL`, so the frame prints without it). The frame's text, and the names
  of its `within` columns and its order key as written (`timeline_names`, `order_name`,
  for messages), are recorded too. The frame's text is a source
  fragment on the aggregate, so every message and plan prints it as written; the fragment
  is recorded while binding, not by `TagDecideSourceFragments`, for the reason the `per`
  key's is (§4a above).
- An aggregate-shaped marker, rather than a wrapper around the payload, keeps every later
  layer from reading the payload as a term of the anchor row; the layers that read
  reducers tell a frame's marker from one (`TryGetDecideFrame`,
  `../04_canonicalizer/done.md`). The planner refuses a range just before
  canonicalization (*"DECIDE: a range frame selector is not supported yet: <frame>"*,
  `plan_select_node.cpp`) until phase 5 formulates it.

---

## 5. Objective

`DecideObjectiveBinder` binds `MAXIMIZE` / `MINIMIZE` bodies through the same
`DecideBinder` rules. It knows the sense (`decide_sense`) because a few checks
depend on it. A scope in front (`BindScopedObjective`) is bound as §4a describes, and
a nested reducer as in a constraint. It performs no algebraic
rewriting: the objective's shape — its additive spine, its reducer scales, its
constant offset — is decided once by `DecideCanonicalizer::CanonicalizeObjective` on
the bound tree, alongside constraints.

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
| Shared DECIDE expression rules, reducers, key scopes (`FindOrCreateKeyScope`, `FindOrCreateExactScope`), where scopes and frames may stand (`ValidateDecideScopeSpellings`, `ValidateFrame`), frame binding (`BindDecideFrame`) | `src/planner/expression_binder/decide/decide_binder.cpp` |
| Degree — the one definition, and the gate over it | `src/planner/expression_binder/decide/decide_degree.cpp` |
| `SUCH THAT` binding and a constraint's scope (`BindScopedConstraint`) | `src/planner/expression_binder/decide/decide_constraints_binder.cpp` |
| Objective binding and its scope | `src/planner/expression_binder/decide/decide_objective_binder.cpp` |
| Key scope struct | `src/include/duckdb/planner/operator/decide/logical_decide.hpp` |
| Scope enum and DECIDE tags | `src/include/duckdb/common/enums/decide.hpp` |
