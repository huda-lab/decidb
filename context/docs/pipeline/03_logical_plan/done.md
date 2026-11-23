# Stage 03 — Logical plan

Represents the decision query and its relational inputs as a `LogicalDecide`
operator sitting above the scan and filter operators of the child plan. It owns
where DECIDE metadata lives, how new constraints and objectives may enter after
planning, and how the whole thing serializes.

It contains no solver-specific structures and no execution mechanics.

**Key source files**

- `src/planner/binder/query_node/plan_decide.cpp` — `Binder::PlanDecide` builds the operator;
  `CreatePlan(BoundSelectNode &)` calls it between the `WHERE` and the aggregates
- `src/planner/operator/decide/logical_decide.cpp` — the operator itself
- `src/include/duckdb/planner/operator/decide/logical_decide.hpp` — the metadata contract

**`BoundDecide`** (`logical_decide.hpp`) is the bound DECIDE part of a query: the decisions (`variables`,
`is_boolean_var`, `variable_scopes`), `keys`, `keyed_scopes`, `frames`, `clauses` with their
`constraint_sources`, `objective`, `sense`, `objective_constant_offset`, `source_text`, `signatures` and
`computed_values`.

`clauses` is a `vector<BoundDecideClause>` (`decide_tree_terms.hpp`), one record per row of SUCH THAT, in written
order with the rows a rewrite adds after them; `objective` is one more record, with a null `body` when the query
writes none. No expression joins the records: a pass loops over the list, and a rewrite touches the record it
changes. A record holds

| Field | What it is | Written by |
|---|---|---|
| `body` | the comparison, the bound `IN`, or the TRUE placeholder a rewrite leaves where a lifted clause stood; for the objective, the minimized expression | binder, canonicalizer, rewrites |
| `scope` (`DecideClauseScope`) | `when`: the rows the row keeps; `per` and `class_key`: its `per K:`; `signature`: its variant signature (D70) | binder; `signature` by the canonicalizer |
| `per_location` | where a written `per` stands in the query, from which its key is quoted | binder |
| `origin` (`DecideRowOrigin`) | the source clause (an index into `constraint_sources`), the removal group, and whether a rewrite made the row for the objective | `InitializeConstraintSourceInfo`, `TagAtomicRemovalGroups`, rewrites |
| `between` | which comparison of a `BETWEEN` the row is | binder |
| `shape`, `group_key` | the row shape (`CanonicalConstraintClass`) and, in the aggregate shape, the `by` key its decision reducers share | canonicalizer, once (`ClassifyClause`); `RewriteScopedReducers` for the clause it leaves |
| `structural`, `defines` | a row a rewrite added to define an auxiliary, and the auxiliary | rewrites |
| `minmax_easy` | an easy-direction MIN or MAX that keeps its reducer | `RewriteMinMax` |
| `folded_bound`, `folded_clause_rows` | a bound folded into the column's box, and whether a repair loosens it through the clause's rows | `FoldSingletonRows` |
| `bound_read_shape`, `bound_read_type` | what bound reduction relied on (`../05_optimizer/done.md`) | `RewriteRhsAggregates` |

A fact of a row is a field of its record, so no alias tag carries one and a copy of a comparison's alias copies none.
The rows a function owns (an L0 indicator's links, an ABS's envelope rows, a body helper's definition) are records
too, held by their function. The binder fills `BoundSelectNode::decide`, `Binder::PlanDecide` canonicalizes it in place and
moves it whole into `LogicalDecide::bound`. The operator keeps what is its own beside it: `decide_index`, the
objective's texts, `source_columns`, `optimized`, the solver choice and the tables the rewrites fill.
`BoundDecide::EnumerateExpressions` is the one list of its expressions; subquery planning (`PlanSubqueries`) and
`LogicalDecide::EnumerateExpressions` both call it.

**The bound expression classes.** Inside a clause's `body` two classes are DECIDE's own; everything else is DuckDB's.

| Class | What it is | Holds |
|---|---|---|
| `BoundDecideReducerExpression` (`planner/expression/bound_decide_reducer_expression.hpp`) | a reducer (`sum`, `avg`, `min`, `max`, `count`, `norm`), and the aggregate a frame binds to | `function` and `bind_info` (the overload `FunctionBinder` picked; `function` is null for a norm no definition has replaced), `children` (none for `count(*)`, two for a collated MIN or MAX), `filter` (the rows it reads); and its facts: `by` (an index into `keys`), `per` (an index into `keyed_scopes`), `norm` (`DecideNorm`: order and the L0 bound), `written_name`, `count_scale`, `holds_reducer`, `retained`, `frame` (an index into `frames`, for the aggregate a frame binds to), `frame_aggregate` (an index into `LogicalDecide::frame_aggregates`, for a reducer over data whose argument reads a frame) |
| `BoundDecideNullTestExpression` (`bound_decide_null_test_expression.hpp`) | an `IS NOT NULL` test a reducer's rows gain beside its written `when` (`DecideCanonicalizer::SkipNullData`), and the test around a recorded reducer's value (`RhsBoundRewriter::InKey`) | `test`: the `IS NOT NULL`, or TRUE once the statistics prove it |

- **The reducer is laid out as DuckDB's `BoundWindowExpression` is**: the bound aggregate function with its bind data,
  its arguments, and the clause-like parts as fields. It is no SQL aggregate: no `LogicalAggregate` computes it and
  DuckDB's executor never reaches it. The tree lowers it, and where DuckDB runs an aggregate (a tree G node, a frame
  column, a direct rule's plan) `ToAggregate()` makes the `BoundAggregateExpression` it names.
- **`Equals` compares what the reducer computes**: its type, function, arguments, rows and bind data. A norm with no
  function is the norm its order and bound name, so it does not equal the SUM over its body. The facts the rewrites
  record (`by`, `per`, `count_scale`, `holds_reducer`, `retained`, `frame`, `frame_aggregate`, and the norm a
  definition was written as) are left
  out, as DuckDB leaves out an alias, and `FactsEqual` (`DecideFactsEqual` for any two nodes) compares them where they
  are structure. `written_name` is display only and is in neither.
- **Each owes DuckDB the obligations of an `Expression` class**: a case in `ExpressionIterator::EnumerateChildren`
  (the reducer's `children` and `filter`; the test), a no-op case in `LogicalOperatorVisitor::VisitExpression`,
  `IsFoldable()` false, and copy, equality, hash and serialization (`expression.json`; the reducer's body is written by
  hand, as `BoundAggregateExpression`'s is, because a function is serialized by name). The NULL test has one more:
  `ExpressionExecutor` executes it by forwarding to `test`, since it stands in filters DuckDB runs.
- **The NULL tests are found by class.** `StatisticsPropagator::FoldSkipNullTests` (the `LOGICAL_DECIDE` case,
  `propagate_decide.cpp`) replaces a test the statistics prove with its constant, as DuckDB folds a filter's
  `IS NOT NULL`. When the tree is built, a test that `KeepsEveryRow()` is dropped and the others are split from the
  written conditions into `DecideNode::member_tests`, so stage 08 reads no tag and no filter shape to tell them.

**A value read whole is a table entry.** `computed_values` holds each flattened scalar subquery and each reducer over
data the objective reads as one value (§2), and a read is told by equality with its entry.

**What a rewrite keeps is an entry of one table.** `LogicalDecide::helper_definitions` is the symbol table of the
constructs the rewrites kept and gave a helper decision (`DecideHelperDefinition`), in the order they were made:
innermost first, so a definition reads only helpers of definitions before it. Every entry has one head (`kind`,
`helper`, `body`, `when`, `origin`, `label`) and its kind's own fields.

| Kind | The construct | `helper` | `body` | Its own fields |
|---|---|---|---|---|
| `ABS` | `abs(e)` over a decision, kept where it is written | the decision that stands for `abs(e)` | the operand | `exact`, `scope` |
| `PRODUCT` | a written product of two forms over decisions, kept where it is written | none | its expansion: the product distributed over its factors' terms, each product with a Boolean factor read as that `BOOLEAN_PRODUCT`'s helper | — |
| `BOOLEAN_PRODUCT` | a product of two decisions, one of them Boolean | the decision that stands for the product | none | `bool_factor`, `other_factor` (the Boolean decision and the other) |
| `L0_INDICATOR` | one counted row of `norm(e, 0)` | the indicator | none | `links` (its three rows, clause records), `m`, `auto_m` |
| `DEFINITION` | a reducer's nonlinear body, kept where it is written | the decision the reducer reads in its place | the body | `relation` |
| `REDUCER_VALUE` | a decision reducer the value rewrite or a range frame replaced by a helper | the decision that holds its value | the reducer node (`Reducer()`), held once | `agg_name`, `path_key`, `class_key`, `push` |

- **A kept call names its definition by a typed reference.** `BoundFunctionExpression::definition`
  (`DecideHelperRef`: the entry's kind and its index) is a symbol reference, as Clang's `DeclRefExpr` is to its
  `Decl`. `LogicalDecide::NameDefinition` is its one writer and `DecideDefinitionOf` its reader. `Copy` carries it;
  `Equals` and `Hash` leave it out, as they do an alias; `DecideFactsEqual` compares it; `DecideStripTags` unsets it on
  the copy `ExpressionBuilder::Of` takes. A call that is first a `PRODUCT` and then a reducer's body names the `DEFINITION`.
  A `REDUCER_VALUE` has no kept call: the reader holds the helper's column.
- **What a row is to the helper it defines is a clause field.** `BoundDecideClause::helper_row` (`DecideHelperRow`:
  `ABS_ENVELOPE`, `ABS_NEGATED_ENVELOPE`, `L0_BOUND`, `L0_TOLERANCE`, `DEFINITION`) beside `defines`, the helper;
  the tree's facts and `DecideConstraint` copy the field.
- **One loop reads the table.** `LogicalDecide::EnumerateExpressions` (every expression execution evaluates),
  `EnumerateBodies` (every body a rewrite reads, with its owner's push and rows: `DecideOwnedBody`, whose `Owner` is
  `CLAUSE`, `OBJECTIVE` or `HELPER` with the entry's index) and the tree's registration each visit the entries in
  table order and switch on the kind. A `REDUCER_VALUE`'s reducer is handed out as its parts (its argument and its
  `filter`), so no walk reads the held node as a reducer still to be rewritten.
- **A fact joined over a helper's readers is a field of the entry.** A `REDUCER_VALUE`'s `push` (`ExtremumPush`) is
  the way its readers push the helper; a reader that shares the helper widens it with `JoinExtremumPush`, and
  `NeedsEnvelope()` and `NeedsClosing()` read the sides off it. An ABS's `exact` is the same kind of fact.
- **None of it is serialized**: only a plan the DECIDE optimizer has not run on is (§5).

**Where the rewrites' structure departs from `context/opa_rewrite/design.md` § 3.2.3.**

1. *There is no `BoundDecideFunctionExpression`.* The kept call stays a `BoundFunctionExpression` and holds a typed
   reference into the table (the symbol-reference pattern above). A wrapper node would hide the call from every
   reader that matches it by shape (the term splitter, the product form, the canonicalizer's operator rebuild), and
   two readers hold a call and no table (`DecideDegreeOf`, the canonicalizer's `HoldsReducer`), which is why the
   reference carries the kind beside the index.
2. *The six row marks are one clause field*, `helper_row`, an enumeration on the record (the design's rule that a
   fact of a row is a field of its record), not facts of a function node: the rows are clause records the tree reads
   one at a time, and a row is one thing to one helper.
3. *A product is two kinds.* `PRODUCT` is the kept call's entry, whose body is the expansion and which has no helper;
   `BOOLEAN_PRODUCT` is one entry per Boolean product the expansion reads, each one helper. One group record would be
   an entry with any number of helpers, where every other entry has at most one.

**Where the bound structure departs from `context/opa_rewrite/design.md` § 3.2.2 and § 3.5.**

1. *A member NULL test is its own node, not a field of the reducer.* `BoundDecideNullTestExpression` is a Decorator
   over the test, as PostgreSQL's `RestrictInfo` is over a conjunct of a qual: it travels inside the conjunction and
   executes as the test. A helper row inherits the tests of every reducer above it, so a field would have had to become
   the type of every rows predicate (a clause's `when`, a term's filter, a link's rows), each a pair. With a node the
   filter stays one conjunction on every carrier and a reader asks the class.
2. *A value read whole is an entry of `BoundDecide::computed_values`, not a node.* This is the design's own rule that a
   shared thing is a table named by index (R25), as `keys` and `frames` are. A wrapper node would sit between a
   flattened column and everything that matches a bound by shape (`StripCastsForIdentity`, `UnwrapDecideCasts`,
   `KeyHolds`), each of which would need a look-through, and DuckDB's executor would need a case for it.
3. *Every DECIDE aggregate, a frame's included, is one `BoundDecideReducerExpression`*, as DuckDB's
   `BoundWindowExpression` is one class for every window function. One bind path makes them all
   (`DecideBinder::BindAggregate`; `BindFrameAggregate` for a frame's), and a frame's aggregate carries reducer facts,
   so a second class would be a two-class case in every function that reads a reducer. The design's
   `BoundDecideFrameExpression` is therefore the field `frame`: a frame class would hold a frame index over a payload
   and still need the function, the bind data, the rows and the facts a range's reducer has, and would owe the
   obligations of a class for them. `TryGetDecideFrame` (`decide_reducer_path.hpp`) is the one test: the class and the
   field. A frame column is another class, a `BoundReferenceExpression` the lowering makes, so the two are not
   confused.
4. *A reference to a frame column is told by its position, with no mark*: its name is the frame as written, which a
   message prints as it stands and not as an identifier. The reference is a chunk column DuckDB's executor reads, a
   `BoundReferenceExpression`, which has no field for the fact. A source column's reference, which DuckDB's
   `ColumnBindingResolver` makes with the column's bare name, lies below the source's column count, and every column
   the tree computes lies past it, so the render of a physical plan reads the count
   (`DecideSourceNames::source_columns`, set by `PhysicalDecide::Names`) and prints a name past it as written.
- `src/execution/column_binding_resolver.cpp` — the `LOGICAL_DECIDE` case
- `src/execution/physical_plan/plan_decide.cpp` — logical → physical

---

## 1. Placement

`LogicalDecide` is inserted **above** the source scan and any `Filter`, so the
solver only ever sees rows that survived `WHERE`. A `Projection` sits above it and
prunes auxiliary variable columns from the user's result.

`GetColumnBindings()` returns the child's bindings plus one per entry of
`decide_variables` — **including auxiliaries**, because constraint and objective
expressions reference them and column-binding resolution has to find them.
`ResolveTypes()` mirrors that.

---

## 2. Subquery planning and correlation provenance

In `Binder::PlanDecide` (`plan_decide.cpp`), and this ordering is load-bearing.

`PlanSubqueries` flattens scalar subqueries into joins. After flattening, a
row-varying column, an **uncorrelated** subquery and a **correlated** subquery are
all indistinguishable — each is a plain `BoundColumnRefExpression`, and the
flattened subquery is even literally named `SUBQUERY`. Correlation is visible only
*before* flattening.

So the operator is built in this order:

1. **Collect**, from constraints **and the objective**, every `BOUND_SUBQUERY` of
   `SubqueryType::SCALAR`, partitioned by `IsCorrelated()`. The owning *slot* is
   held, not the node — `PlanSubqueries` replaces the node in place.
2. **Flatten** with `PlanSubqueries`.
3. **Read back** what each slot became and **record** a copy of it as an entry of
   `BoundDecide::computed_values` (`DecideComputedValue`, `decide_tree_nodes.hpp`): the `value` a clause reads,
   `query_wide` for an uncorrelated subquery, and `subquery`. The table is one of the expressions
   `BoundDecide::EnumerateExpressions` lists, so DuckDB's passes rebind an entry with the clauses, and a read of the
   value equals its entry (`Expression::Equals`) wherever a rewrite copies it. `IsDecideQueryWideValue` and
   `DecideHoldsSubqueryValue` are the two lookups; the canonicalizer, the optimizer and the tree hold the vector by
   reference, so an entry a rewrite appends is seen.
4. **Canonicalize** constraints and objective, then verify (stage 04), then run the shape check on the canonical
   spelling (stage 04, `../04_canonicalizer/done.md` §3.11).

The objective is collected too, and must be: an uncorrelated `(SELECT k) * MAX(x)`
is otherwise indistinguishable from a row-varying column and hits the fail-safe
default, which rejects it for varying per row when it does not.

A correlated subquery's entry (`query_wide` false) never changes a decision — a correlated
subquery is row-varying by construction and the fail-safe default would reject it
anyway. It exists so a message about a bound that holds one states the evaluated number instead of
`SUBQUERY`.

### Step 2 flattens unconditionally, including inside another flattening

`Binder::PlanSubqueries` normally *defers* a subquery it meets while an outer
subquery is still being flattened: it sets `has_unplanned_dependent_joins` and
leaves the node in place, and `RecursiveDependentJoinPlanner` finishes the job once
the outer subquery has been peeled. Ordinary clauses tolerate that because nothing
reads their expressions in between.

A DECIDE clause does not. Step 5 runs in this same `CreatePlan`, and
canonicalization copies the clauses — and `BoundSubqueryExpression::Copy()`
throws by design. So the DECIDE block saves `is_outside_flattened`, forces it true
across the two `PlanSubqueries` calls, and restores it. Layer 4's input contract is
a subquery-free bound tree, so the flattening is not optional at this point.

Without it, *any* subquery written inside a nested DECIDE fails with
`Serialization Error: Cannot copy BoundSubqueryExpression` — a DECIDE nested two
deep, but equally a plain scalar subquery used as a bound inside a single nested
clause, or one scaling its objective. Only the outermost `CreatePlan` escapes this,
because it is not itself inside a flattening. Pinned by
`test/decide/tests/test_nested_decide.py`.

---

## 3. The two post-planning entry points

After planning, `bound.clauses` and `bound.objective` change only
through these three methods. Appending a record or assigning the objective directly bypasses
canonicalization and verification.

```cpp
void LogicalDecide::AddClause(ClientContext &, BoundDecideClause);
void LogicalDecide::CanonicalizeClause(ClientContext &, BoundDecideClause &);
void LogicalDecide::SetObjective(ClientContext &, BoundDecideClause);
```

`AddClause` canonicalizes and verifies the incoming record, then appends it. `CanonicalizeClause` does the same for
one record of the list a pass rewrote where it sits. Each reads one record, so a rewrite costs the clause it changes
and never the list.

`SetObjective` canonicalizes and verifies the replacement. Its peeled constant is
**added** to `objective_constant_offset` rather than assigned, so the constant the
user wrote survives every later optimizer rewrite. A null objective clears the
field.

Together with the call in `Binder::PlanDecide`, these are the only places
`DecideCanonicalizer` canonicalizes a clause or the objective.

---

## 4. What `LogicalDecide` carries

Grouped by who writes it:

**From the binder** — `decide_index`, `bound.variables`, `bound.clauses`,
`written_sense`, `bound.objective`, `is_boolean_var`,
`variable_scopes`, `keys`, `keyed_scopes`, `constraint_sources`. `keys` is the one
key table (`DecideKey`): every key the clauses write -- a declaration's `per K:`, a
reducer's own `per K'`, a reducer's `by (G)`, a
constraint's `per K:` and a nested reducer's path key -- is one entry, and keys with
the same elements share it, whichever spelling wrote them (`FindOrCreateDecideKey`
compares elements as a set). An entry holds the key as first written and its elements
(`DecideKeyElement`: the bound expression, its name as a message prints it, and its
`DecideKeyNullMode` -- `VALUE` for a column a relation element stands for, `NO_GROUP`
for one written on its own, which the written one keeps when both name it), sorted (columns
by binding, then other expressions by text), and `written_as_by` when a `by` names it. The
empty key (`per ()`, `by ()`) has no entry: it is `INVALID_INDEX`. A key holding
a subquery is recognised by its text instead, since bound subqueries do not compare. A reducer's
`by` field (`BoundDecideReducerExpression::by`) names its `by` key, and a clause's `scope.class_key` its `per` key
(`INVALID_INDEX` is
`per ()`).
`keyed_scopes` holds the key scopes, one per key that keys decisions or a reducer's own
`per` (`KeyedScopeInfo`: the key, the decisions it keys). `variable_scopes` points each
keyed decision at one, with its key as written (`declared_key`). A scope the optimizer
creates for a helper decision of the value rewrite has the link's `by` key and names the
link (`KeyedScopeInfo::value_link`, deliberately absent from the serialization: an
optimized plan is never serialized); its entities are the link's `by` groups among the
rows the link's clause keeps, so it shares no scope with a binder scope on the same key.

**From the canonicalizer** — `objective_constant_offset`, and the variant signatures
(D70): `signatures`, one `DecideSignature` per scoped expression with a field (the rows
it reads, its `per` key as an index into `keys`, its fields, the text a plan names it by,
its clause), indexed by a `per K:` clause's `scope.signature`, by a reducer's
own scope (`KeyedScopeInfo::signature`, a scope the canonicalizer appends for a
relation-qualified reducer over decisions whose body has a field), and by the objective's
`scope.signature`, `INVALID_INDEX` when it has no field. A `DecideSignatureField` is a
data `value`, a `decision` (an index into `decide_variables`), a `by` key (an index
into `keys`) or a `frame` (an index into `frames`, D82), with its text. `frames` holds one
`DecideFrameSpec` per frame selector (`../../language/guide.md` §5.6, property 223),
indexed by the `frame` field of the aggregate it binds to: its anchors and their
`k`, `every`, `all`, its fallback and order key, `desc`, `cyclic`, and its `within` key as
an index into `keys`, and a range's reducer (id 121); then where the canonicalizer placed it (stage 04, ids
113-120): `rows`, the rows its timelines read (its path's, null for every row), `timeline_key`, the names its
messages use, and `partition_key` with `partition_subject`, its D75 check. Whether it has a value at an anchor is the
data's to say (stage 08). `value_checks`
holds the value checks the bound-side rewrite and the ABS rewrite append (stage 05).
`PlanSubqueries` runs on the key elements, the frames' order keys and fallbacks as on
the trees, and `EnumerateExpressions` visits the key table, the frames, the signatures
and the checks, so binding resolution and column pruning see their columns.

**From the optimizer** — `num_auxiliary_vars` (the decisions it appends after the declared
ones), `solver_backend_name`, `use_native_constructs`,
`force_native_constructs`, `helper_definitions` (the one table of the constructs the rewrites kept, above),
`aux_var_expressions`, `rewrite_accounts` (one fact per construct a rewrite
replaced -- the construct as a plan prints it and what it became, `RewriteAccount::Kind`
-- under the clause it came from, INVALID_INDEX for the objective; worded by the renderer;
never serialized), and the folded variable box. A `REDUCER_VALUE` entry of `helper_definitions` is the value
rewrite's helper decision, one per reducer over one set of rows, shared by every clause that reads it: it holds the
reducer the helper replaced (the node, with its own `when` and `per`), its path key (its `by` key under every enclosing
one), its SQL as a label, the rows its path keeps, and the way its readers push it; its body's terms are lowered into
its node's `DecideBody::members` (`DecideTermLowering::Members`); every MIN or MAX over decisions is one.

`optimized` marks the boundary between the first two groups and the third. It is
set before the optimizer's first rewrite and is what takes this plan off the wire —
see §5.

### The solver is named, not held

Stage 05 has to settle three solver questions before it rewrites anything (see
[`../05_optimizer/done.md`](../05_optimizer/done.md) §0), and every answer rides the
plan from here. What the plan carries is the *answers*, never the solver:

- `solver_backend_name` — the registered identifier (`"gurobi"`, `"highs"`).
- `use_native_constructs` — a `SolverConstructSupport`, which constructs this backend
  can state itself.
- `force_native_constructs` — the policy over them: whether every declared construct is stated natively (test-only),
  or a construct is lowered where its lowering is measured faster. It defaults to false.

The third is a policy rather than a fact, and that is deliberate. Applying it needs a
per-clause test on evaluated coefficients, which no stage before execution can run — so
the plan carries the *rule* and stage 08 carries it out. That is not the plan deferring
a decision; it is the decision arriving where the data it ranges over exists.

A live `SolverBackend` handle here would let a logical plan open a solver session,
and session creation is stage 07's. A name cannot: turning it back into a backend is
a registry lookup (`SolverRegistry::Find`), and that happens where a solve is about
to run — `PhysicalDecide::PlannedSolverBackend()` — not on the plan.

None of the three is serialized. Which solver a host has is a fact about the host, not
about the query, so a plan replayed elsewhere re-resolves them.

### The key table keeps its columns alive

Every key element is an expression `EnumerateExpressions` visits, so DuckDB's column
pruning keeps a key column a declaration alone names (`per region: x`), and binding
resolution turns it into a chunk position like any other expression. Without that, a key
column not referenced in `SELECT` / `WHERE` / the clause would be pruned from the scan,
collapsing distinct entities into whatever grouping happened to survive. Execution reads
the key's resolved elements directly (`DecideKeyElements` over the tree's `keys`).

A key only a declaration writes names no column for a message: `plan_decide.cpp` names
the columns a clause writes, a `per K:` key's as written, a `by` key's as it binds, and
leaves a column only a declaration's key reads unnamed, as it leaves any column no clause
names.

### Column binding resolution

`ColumnBindingResolver`'s `LOGICAL_DECIDE` case enumerates DECIDE's
expression-holding fields **explicitly** rather than calling
`VisitOperatorExpressions`, because DECIDE variables must be shielded from
resolution via `ignored_bindings`; it visits every field through
`LogicalDecide::EnumerateExpressions`, the one list of them: `bound` (`variables`, `constraints`, `objective`, `keys`,
`frames` with order key, fallback and rows, `signatures` with rows and fields' values), `helper_definitions` (each
entry's body or, for a `REDUCER_VALUE`, its reducer's argument and filter; its `when`; an L0 indicator's links) and
`value_checks`.

**Any new field holding expressions that execution will evaluate must be added to
`EnumerateExpressions`.** An optimizer rewrite that moves subexpressions out of
`bound.objective` into a side vector (`helper_definitions` is one)
otherwise silently opts them out, and the operator then reads a logical index as a
chunk position. A single-table source hides the bug entirely, because the two
indexings coincide there; it surfaces only once a join reorders columns.

Arms kept as guards (BLOAT-28): `DecideSourceColumnKey`'s `slot >= column_ids.size()` (a scan with no column ids
answers the binding `(table_index, 0)`, which names no column) and `EnumerateExpressions`' null `constraints`
check (the rewrites detach the clause and reinstall it). No query reaches them.

---

## 4b. The `DIAGNOSE` flag, and the operator that reads it

`LogicalDecide::diagnose` is a plain `bool`, set by `Binder::BindDiagnose`
(`bind_showref.cpp`) when the statement carried the `DIAGNOSE` prefix, and copied to
the tree's `diagnose` at physical planning (`plan_decide.cpp`). It is the ONLY thing
that arms the diagnostics engines.

Two properties matter, and both are the reason it lives here rather than in a session
setting:

- **It is a property of the statement.** Two queries on one connection can differ, and a
  plan carries its own answer. Nothing downstream reads it back out of a setting; layer 8
  reads the field it was handed, the same way it reads `solver_backend_name`.
- **It travels down, never sideways.** parser → binder → this operator → stage 08. The
  binder does not consult the executor, and the executor does not consult the session.

`BindDiagnose` binds the inner query exactly as it would without the prefix — DIAGNOSE
changes what is reported, never what is asked — then `CollectDecides` walks the complete
bound plan. Zero DECIDE operators is an error. More than one is also an error, naming the
count and asking the user to diagnose each decision query separately. With exactly one,
the binder sets that operator's flag and wraps the whole plan in
`LogicalDecideDiagnose`. This applies equally when the sole DECIDE sits inside an
ordinary subquery; what is refused is ambiguity between several solves, not nesting.

**`LogicalDecideDiagnose`** (`planner/operator/decide/logical_decide_diagnose.hpp`) is the
prefix's own plan node: one child (the whole decision query), and an output schema that
is *not* the child's. It resolves its types from `GetDecideDiagnoseSchema` — the single
definition of the diagnosis relation's columns, shared with the physical operator that
fills them. The child's rows never surface: DIAGNOSE reports on the run, it does not
return the run's output.

A query with no `DECIDE` clause is rejected here, at bind time, rather than returning an
empty relation. That is the right layer for it: whether a plan contains a decision is a
binding fact, and the parser cannot see it (the clause may sit inside a subquery).

## 5. Serialization

`LogicalDecide` and `LogicalDecideDiagnose` are serialized by DuckDB's generator.
The field list lives in `storage/serialization/logical_operator.json`; the structs
it names — `DecideKey`, `DecideKeyElement`, `KeyedScopeInfo`, `DecideVarScopeInfo`,
`ConstraintSourceInfo`, `DecideSourceColumnName`, `DecideSourceText` — have their own entries
in `nodes.json`, and both
directions are generated into `src/storage/serialization/serialize_*.cpp`. There is
no hand-written copy. Adding a field means adding one JSON entry, not two mirrored
lines in two functions that can drift apart; `python3 scripts/generate_serialization.py`
with no arguments rewrites the generated files in place (`--target` alone is ignored).

Nothing is flattened into parallel vectors. A key is serialized whole -- its text and
its elements, each with its expression, name and NULL mode -- and a scope or a check
holds the key's index. Source-column display metadata is likewise a `vector<DecideSourceColumnName>`:
each record keeps a `ColumnBinding` and its user-written name together from binding,
through serialization, to physical planning.

### The wire carries bound plans only

The format carries what the binder and the canonicalizer produced. Everything
`DecideOptimizer` writes stays off it: `LogicalDecide::optimized` is set before the
first rewrite runs, and `SupportSerialization()` reports false from that point on.

That is a claim about portability, not about cost. Stage 05 chooses a formulation
from the constructs **this host's** solver declares — the same reason
`solver_backend_name` and `use_native_constructs` are not serialized (§4). An
optimized DECIDE plan is an answer computed for one machine, so it declines to be
copied rather than arriving somewhere else as a formulation that machine cannot
honor. The helper decisions, the folded variable box and the prepared
linear form are all rebuilt from the canonical tree by the optimizer, in
milliseconds, wherever the plan lands.

`Planner::VerifyPlan` runs twice — on the bound plan (`Planner::CreatePlan`) and again
at the end of `Optimizer::Optimize`. The first round-trips DECIDE; the
second asks `OperatorSupportsSerialization` and is told no, so it returns without
touching the plan. `LogicalOperator::Copy` asks the same question and raises rather
than handing back a truncated plan.

### The property ids are a sequence, not a lookup

`BinaryDeserializer::OnPropertyBegin` requires the *next* field in the stream to
equal the id being read. The ids are a running checkpoint that the two sides are
still in step, not keys that can be sought. Write order and read order must match
exactly — which is why they are generated from one list, and why an id may be
renumbered freely: a serialized logical plan lives for microseconds inside one
process and is never persisted, so there is no older layout to stay readable for. A
retired field stays in the list as a `"deleted": true` entry, for which the generated
serializer writes nothing and the deserializer reads past (`ReadDeletedProperty`). `LogicalDecide` writes `clauses` at 202, `objective` at 204, `signatures`
at 223 and `frames` at 225, `KeyedScopeInfo` its `signature` at 108; a `BoundDecideClause` (`nodes.json`) writes
every field the binder and the canonicalizer fill.

### What exercises it

`PRAGMA verify_serializer` round-trips the bound plan and **replaces the live plan
with the copy**, so a field that never reaches the wire becomes a wrong answer
rather than a silent omission. Run over the existing suite, that is a guard nobody
has to maintain per field:

```bash
DECIDB_VERIFY_SERIALIZER=1 make decide-test
```

`DECIDB_VERIFY_SERIALIZER=1` makes `test/decide/decidb_cli.py` attach the pragma to
every non-interactive query it runs through shell startup commands, with the pragma's
own result redirected away. Parsed DECIDE statements reparse from `ToString()`, with no
second solve; see [`../01_parser/done.md`](../01_parser/done.md) §5. The two PTY-driven
interactive continuation cases are outside DuckDB's materializing verifier. Model-dump
assertions select one complete build when verifier paths append equivalent copies. The parsed verifier re-parses an
`EXPLAIN` from its printed text, so an error under `EXPLAIN` loses its caret in this mode; tests that compare an
`EXPLAIN` error with a run's compare the error's first line.

---

## 6. EXPLAIN

`GetName()` returns `"DECIDE"`. `ParamsToString()` builds the structured plan
output from the clause records (`CollectDecideClauseLayers`), printing every clause in one style
(D32): each clause as typed, its `when` and `per` as one qualifier in front (`when c per K:`), and the
defaults `per row:` and `by ()` not printed (`../../language/explain.md`, Rendering). A nested reducer prints as written,
`outer(per k: inner(e) by (k))`, a `when` in front of it, each key as written: `RenderDecidePerKey` cuts the key from
`DecideSourceText` at the record's `per_location`. A row the solver receives is left out where it reads as
the clause above it. The renderers read the clause
registries and the auxiliary labels (`aux_var_expressions`, with `decide_index`), so a decision
column that stands for an expression -- an ABS auxiliary `abs(e)`, a product `(b * x)`,
an indicator `(x = v)`, a helper `sum(e) by (g)` -- prints that expression wherever it
appears, a later rewrite's label included. Each
rewrite's account (`rewrite_accounts`, worded once by `RenderDecideRewriteAccount`:
"`abs(e)` becomes a helper decision"; it names what the expression became, never the
solver's construction) is printed under its clause before the rows it left; the objective's accounts, and the rows a rewrite added for the objective
(`origin.objective`, returned by `CollectDecideClauseLayers` in `objective_rows`), print
under the objective. A MIN or MAX prints as written in a clause and in the objective, its account under it; an AVG
objective prints its sum under its account; an
L0 link's constants print by the names the user writes or sets them with: the bound of an `L0_BOUND` row
(`BoundDecideClause::helper_row`) as `M` when automatic (`DecideHelperDefinition::auto_m`) and as the number in
`norm(e, 0, M)` when written, and the tolerance of an `L0_TOLERANCE` row as `decide_l0_tolerance`. A rewrite's row prints its clause's scope as written
(`per region:`, and no `when true:` the query did not write; `DecideObjectiveQualifier` for the objective's
rows), drops a zero the rewrite wrote (`IsRewriteZero`: a numeric constant with no query location, so `0 + v`
prints `v`), and prints a DOUBLE constant the rewrite wrote as a plain number (`RewriteNumberText`: `>= 0`).
An `IN` under
a scope keeps its clause's qualifier in its source info, so a suggested edit pastes back with its scope; an infinity norm
echoes its order quoted (`norm(e, 'inf')`). The diagnosis label of a MIN/MAX helper
(`DecideHelperDefinition::label`) is spelled as the plan prints the clause (`max(ship)`), and a `<>`'s binary is labelled by
its clause as written (`QuoteWrittenClause`). A quoted span is parenthesized by how
tightly its own text binds (`DecideBinding`, recorded with the span), not by the bound node. In a reprinted node,
parentheses follow DuckDB's precedence: a cast with no location passes its precedence to the child it hides, the right
operand of a non-associative operator keeps its parentheses (`x * (cap % 7)`, `a ** (b ** c)`), a negated negative
prints `-(-x)` and an `OR` under an `AND` keeps its parentheses. A reprinted function prints its lower-case name and a
column is quoted where SQL needs it (`"order"`, `"per"`).

The rows an ABS envelope or an L0 link adds belong to the tree's F nodes, not to the constraint list, so the physical
operator reads them there (`CollectDecideFunctionRows`, ruling R4): each F node's clause root prints its terms and bound
as stage 05 lowered them, its scope's `when c per K:` in front, after the clause's other rows. A frame read in such a
row prints as its selector, `else` included (`at(previous else 0: x) over (t.period within t.product)`). The logical
operator holds no tree and prints none of them; `EXPLAIN` prints the physical plan.

EXPLAIN ANALYZE adds what the run made of each clause (`run_time_accounts`). A helper that stage 06 writes where it is
read holds no column (D89), so its line (`Kind::VALUE_HELPER_INLINED`) replaces the plan's, which says it becomes a
helper decision: the clause prints one line about its helper. The variants line (`↳ per t.g: N constraints from M
classes`) prints for every `per` clause that names classes, a clause whose rows merge into `by` groups included
(`DecideModelEmitter::Groups`), and names a helper's reducer as written, its outer `by` included.

Both the walker and the leaf renderer `RenderDecideSource` live in
`src/planner/decide/decide_source_provenance.cpp`, beside the location rule they depend on, because every surface that
echoes a DECIDE clause back to a user needs them: the logical node, the physical node, and the labels an
infeasibility diagnosis prints. `Expression::ToString()` cannot serve any of them: it prints the tree as bound, so it
shows the casts the binder inserted while reconciling types and spells arithmetic in call form. `RenderDecideSource`
decides what to show by **location**, Clang's SourceManager idea: the parser kept a span of the query for every
expression of the DECIDE tail (`DecideSourceText`), a bound node names its span by its query location, and the
renderer cuts the span of a located node (the parentheses around it excluded, its whitespace and comments verbatim)
before it reprints anything. A message quotes every located node. EXPLAIN's written layer does the same; its canonical
and rewritten layers quote every located subtree that reads no decision and reprint the decision structure, which
canonicalization and the rewrites reshape. A node with no location (one a rewrite built) prints its operator and quotes
its children; an implicit cast takes its child's location, so it quotes as the child does. The rule that keeps a
location true is Clang's TreeTransform one (`../architecture.md` §3, `DecideReplaces`).

`source_text` (`DecideSourceText`) rides on `LogicalDecide` (serialized) and is copied to the operator's `display`,
alongside the `constraint_sources` registry that shares the same renderer. A registry entry (`ConstraintSourceInfo`)
stores a clause once: its span (`location`, the query location of its comparison, a BETWEEN's shared by its two halves),
the span of its canonical left side (`lhs_location`), the canonical text a plan shows (`canonical_lhs`, `canonical_cmp`,
`canonical_rhs`) and `canonical_differs`, which the canonicalizer reports (`DecideCanonicalizer::CanonicalizeClauses`). The
clause as written is cut from `DecideSourceText` by `location` where a plan or a message needs it. A clause whose two
sides both read a decision (`ship <= capacity * open`) keeps `source_lhs` and `source_rhs`, DeciDB's own rendering of
the two sides before canonicalization merged them, because the clause a repair edits is no text of the query. The span
id travels to formulation: `source_clause_id` gives a clause-level message its caret (`DecideClauseLocation`,
`DecideClauseErrorInfo`). The objective's `written_objective` is the text cut from the parser's `OBJECTIVE` span; its
`objective_canonical_differs` still compares two renderings.

A clause with a decision on both sides carries `source_lhs` and `source_rhs`, and `DIAGNOSE`'s echo (D32) of those sides
is DeciDB's rendering, not the query text.

See [`../../language/explain.md`](../../language/explain.md)
for the output itself.

---

## 7. Source map

| Concern | Location |
|---|---|
| Operator construction, subquery provenance | `src/planner/binder/query_node/plan_decide.cpp` |
| Operator, entry points, EXPLAIN strings | `src/planner/operator/decide/logical_decide.cpp` |
| Serialization, generated from `logical_operator.json` and `nodes.json` | `src/storage/serialization/serialize_logical_operator.cpp`, `serialize_nodes.cpp` |
| Metadata contract (every field, documented) | `src/include/duckdb/planner/operator/decide/logical_decide.hpp` |
| Binding resolution shielding | `src/execution/column_binding_resolver.cpp` |
| Logical → physical, entity key physical indices | `src/execution/physical_plan/plan_decide.cpp` |
| `DIAGNOSE` binding, and the flag's origin | `src/planner/binder/tableref/bind_showref.cpp` (`BindDiagnose`) |
| The `DIAGNOSE` plan node and its schema | `src/planner/operator/decide/logical_decide_diagnose.cpp` |
| Source display registry | `src/planner/decide/decide_source_provenance.cpp` |
