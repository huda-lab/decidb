# Stage 08 — Physical execution and readback

Runs the relational input, evaluates every prepared coefficient and grouping
against the materialized data, invokes the solver contract, and projects the
solution back onto rows. It implements no rewrites, no backend-specific
formulation, and no algebra: the constraints and objective arrive already
flattened into terms.

`PhysicalDecide` is a **blocking** operator: the optimal value of any one decision
depends on the whole dataset, so it must consume its entire input before it can
produce a row.

**Key source file**: `src/execution/operator/decide/physical_decide.cpp` (~4,970 lines)
**Header**: `src/include/duckdb/execution/operator/decide/physical_decide.hpp`

`Finalize` itself is a short orchestrator; the phases below are private methods
on `PhysicalDecide`, in call order:

| Method | Phase |
|---|---|
| `BuildEntityMappings` | 1.5 — row → class maps for every key scope: a keyed decision's key, a reducer's own `per` or colon form, a helper decision's `by` key |
| `EvaluateConstraints` | 2, constraints — appends to `gstate.evaluated_constraints` |
| `EvaluateObjective` | 2, objective — per-term filters with the objective's `when` folded in, and the two-level form's classes |
| `EvaluateComposedClauses` | 2, composed MIN/MAX — per-row coefficients, reducer factors, filter masks, constant RHS |
| `EvaluateReducerValueLinks` | 2, the value rewrite's helper decisions — each reducer's members on the rows the clause and the reducer keep, grouped by its `by` key |
| `BuildSolverInput` | 3a — assembles `SolverInput`, settles the column box, builds the `VarIndexer` |
| `FormulateModel` | 3b — derives the constants and emits the rows, against a given box |
| `FormulateElasticModel` | 3b again, under `DIAGNOSE` only — the same pass against the widened box a repair searches |
| `FinalizeSolveResult` | 3 — calls `SolveModel`, routes the terminal (`SOLVED`/`UNBOUNDED`/`INFEASIBLE`/`TIME_LIMIT`), and finishes `gstate` |

The three PHASE 2 methods return one `EvaluatedClauses` between them: everything read
off the data that a later phase still needs. It is named for the phase that fills it
rather than for one consumer, because it carries objective terms and composed-MIN/MAX
*constraint* clauses alike.

Coefficient evaluation that both the constraint and objective paths need is
factored into shared file-scope helpers rather than duplicated per path:
`EvaluateBilinearTerms` (bilinear coefficient scan + filter masking, used by
both `EvaluateConstraints` and `EvaluateObjective`) and `ScaleAvgRows` (the
AVG-denominator division, used by every AVG-scaled term — linear, bilinear, or
a quadratic group's inner columns — on both sides). `EvaluatedConstraint::
BilinearTerm` (`solver_input.hpp`) is the one bilinear-term struct; there is no
separate objective-side copy.

---

## 1. Order of operations

| # | Where | What |
|---|---|---|
| 1 | `GetGlobalSinkState` → `DecideGlobalSinkState` ctor | Read the absorbed variable box and alias the prepared linear form |
| 2 | `Sink` / `Combine` | Materialize every surviving tuple into a `ColumnDataCollection` |
| 3 | `Finalize` → `BuildEntityMappings` | Build the row → class maps of the key scopes |
| 3a | `Finalize` → `RunScopeChecks` | Run the scope checks (§4a); the first failure stops the query |
| 4 | `Finalize` → `EvaluateConstraints` / `EvaluateObjective` / `EvaluateComposedClauses` / `EvaluateReducerValueLinks` | Evaluate coefficients, `when` filters, `by` groups and `per` classes, RHS values, the helper decisions' members |
| 5 | `Finalize` → `BuildSolverInput` → `FormulateModel` | Assemble `SolverInput` and settle the box, then linearize against it (stage 06) |
| 6 | `Finalize` → `FinalizeSolveResult` | `SolverModel::Build` → `SolveModel` (stage 07) |
| 7 | `GetData` | Project solution values back onto rows |

The sink state is constructed before the first `Sink` call, so step 1 sees no
data — which is fine, because it derives nothing. Flattening already happened at
plan time (stage 05).

Read consistency follows from step 2: the optimization runs on the snapshot the
query saw. Concurrent modifications cannot affect a running solve.

---

## 2. The absorbed variable box

Whether a simple bound (`x <= 10`) becomes a column box or a model row is decided
at stage 05 — see [`../05_optimizer/done.md`](../05_optimizer/done.md), "Bound
absorption". This stage consumes that decision and does not re-derive it.

The sink state copies `op.absorbed_lower_bounds`, `op.absorbed_upper_bounds` and
`op.user_absorbed_bounds` in its constructor. Three consumers read them:

- `Finalize` copies the box into `SolverInput`, resolving any lower bound still at
  `ABSORBED_LOWER_UNSET` to the default 0 floor — non-negative unless the query
  explicitly said otherwise.
- The domain-contradiction guard below reads the box to reject a user bound that
  contradicts a variable's intrinsic domain, quoting the bound's clause as written.
- `BuildSolverInput` re-opens each `UserBoundSpec`'s direction in the rigid box, which
  `OpenElasticColumnBox` then widens for a diagnosis (below).

Flattening skips any comparison tagged `ABSORBED_BOUND_TAG`, so an
absorbed bound does not also produce `num_rows` redundant per-row model rows. The
comparison itself stays in the tree, which is why `EXPLAIN` still renders the bound
the user wrote. A recorded one is flattened aside into
`DecidePreparedModel::absorbed_constraints`: under DIAGNOSE `EvaluateConstraints`
evaluates it after every other prepared clause (numbered after them) into
`evaluated_absorbed_constraints`, and only the elastic input receives it, so the repair
model loosens it one kept row or class at a time (D28) while the solved model keeps the
box alone. Those clauses are the repair model's only rows for an absorbed bound.

### The elastic box

`OpenElasticColumnBox` turns the solved model's box into the box the elastic model is
*formulated against*: every direction a repair may loosen is widened, so each is enforced
only by the loosenable row the diagnosis emits beside it. Both revertible layers move —
the user bounds stage 05 absorbed, and the tightenings `DecidePropagateImpliedBounds`
derived — which is why the test is simply "is this direction narrower than `rigid_*`?":
propagation only ever narrows, and stage 05's absorption is already re-opened in the
rigid snapshot. A BOOLEAN's [0,1] is intrinsic and never widens past it, so a user pin on
one becomes loosenable while the 0/1 domain stays rigid.

It works on `SolverInput`, before formulation, rather than on a built `SolverModel` —
that is the whole point. The constants that go stale against a re-opened box (Big-M,
McCormick) are derived by the formulation pass, so the box has to change *before* that
pass runs, not after. `PhysicalDecide::FormulateElasticModel` widens the box on a copy of
the retained pre-formulation input and re-runs `FormulateModel` against it, so the
elastic model's constants describe the box it declares.

**The widening is bounded, and bounded per column.** An open column has no finite Big-M, so
a formulation against a fully re-opened box does not lose precision — it refuses the query.
Each loosenable direction is therefore widened by a finite amount taken from that column: ten
times its own magnitude, or whatever a row demands of it (`|rhs| / |coefficient|`), whichever
is larger. No model-wide quantity is involved, so an unrelated large value cannot inflate
this column's constants; a row that genuinely demands a much larger value still can. The
reasoning, and what the looser constants cost in numerical noise, are in
[`../../07_query_diagnostics/infeasible/done.md`](../../07_query_diagnostics/infeasible/done.md)
"The elastic model is a formulation, not a matrix patch".

### Domain contradictions are a static error, not a diagnosis

A user bound that contradicts the variable's *intrinsic* domain is deterministic —
loosening cannot help, changing the type can — so it throws here, in the main
pipeline, before the elastic engine ever sees it, quoting the bound as written
(*"per g: max(x) <= -1 cannot hold because x is non-negative (>= 0)"*):

- `x <= -1` on a non-negative type, guarded as `U < 0 AND L >= 0` so that
  `x = -1` or `x <= -1 AND x >= -5`, which explicitly lower the floor, still pass;
- `x >= 2` or `x = 2` on a BOOLEAN.

A purely user-vs-user inverted box such as `x >= 5 AND x <= 1` does **not** match
and proceeds to the elastic engine, which reports a least-change loosen.

### Implied bounds from constraint data

Once coefficients are numbers, a second, data-driven source of column bounds
becomes available. Stage 08 invokes it — `DecidePropagateImpliedBounds()` — but
does not own it: the pass is pure over `SolverInput` and lives at stage 06. See
[`../06_model_formulation/done.md`](../06_model_formulation/done.md) §9.

The call is ordered here, immediately after the absorbed bounds are resolved and
before any Big-M is computed, because every downstream constant depends on the
tightened box.

The pass is a single sweep, not a fixpoint: a bound derived for one variable is
not fed back to tighten others in the same pass. It skips constraints it cannot
reason about soundly — any negative coefficient, bilinear or quadratic terms,
MIN/MAX and `<>` indicator rows, and a clause with rows outside its row map, such as one
under a `when` (whose bound would wrongly cap the excluded rows, since the derived bound
is shared across all of a variable's rows). A `per K:` clause with no `when` is not
skipped: each row it leaves out stands with its class's first row
(`rows_stand_with_their_class`).

---

## 3. The prepared linear form

Execution does **not** flatten expressions. `LogicalDecide::prepared` arrives
already decomposed into additive terms by `BuildDecidePreparedModel` (stage 05,
`src/optimizer/decide/decide_linear_form.cpp`), moved into
`PhysicalDecide::prepared` at physical planning. `DecideGlobalSinkState` aliases it:

```cpp
const vector<unique_ptr<DecideConstraint>> &constraints = op.prepared.constraints;
const unique_ptr<DecideObjective> &objective = op.prepared.objective;
```

**What this stage receives.** For every constraint: its terms, each naming a
variable index, a sign, a still-unevaluated coefficient `Expression`, an optional
filter (its reducer's own `when`), an AVG-scaling flag and the key scope of its
reducer's own `per` (`qualifier_scope_idx`); plus the RHS expression, the comparison
type, the clause's `when`, its `by` key (`group_key`) and `per` key (`per_key`,
`class_key`), and the metadata linking it to indicators and diagnosis provenance. The
objective arrives the same way, with its `when`, the two-level form's key
(`per_columns`), and `squared_terms` and `bilinear_terms` alongside `terms`.
`ComposedMinMaxTerm::inner_terms` carries the composed MIN/MAX path's terms, so no
construct is left re-deriving its own.

**What this stage does with it.** Evaluates the coefficient expressions against the
materialized rows (PHASE 2), and nothing else. Which variable a term names and what
multiplies it were settled upstream, where a binder was in scope.

**The shape is asserted, never repaired.** A decision variable on the RHS, a
non-reducer term in an aggregate LHS, an unrewritten MIN/MAX — each is an internal
invariant failure raised by the flattening pass at plan time, not something
execution fixes. The structures themselves are documented in
`src/include/duckdb/planner/decide/decide_prepared_model.hpp`; the algebra that
produces them, and why it cannot live here, is
[`../05_optimizer/done.md`](../05_optimizer/done.md) §1a.

---

## 4. PHASE 1.5 — entity mappings

For each `EntityScopeInfo` — a keyed decision's key (`per K: x`), a reducer's own `per`
or colon form (`sum(per K: e)`, `SUM(D: e)`), a helper decision's `by` key — before
coefficient evaluation:

1. Evaluate the entity-key columns per row from the materialized data, using
   `BoundReferenceExpression`s built from `entity_key_physical_indices` so one
   `ExpressionExecutor` covers them all. A helper decision's scope
   (`EntityScopeInfo::value_link`, the value rewrite) has no key columns: its link's
   `by` key expressions are evaluated instead (`CachedTransformToChunkExpression`), on
   the rows the link's clause `when` keeps only (`BuildGroupIds`' `read_rows`), one
   entity per value they take together, NULL a value. A row the clause drops is no
   group; every such row maps to one spare entity, so each row has an id and the key is
   never evaluated where the clause never reads.
2. Concatenate each row's key values into a composite string key with NULL-safe
   tagging, so a real NULL is distinguishable from the string `"NULL"` — the same
   pattern the `by`, class and check keys use (`BuildGroupIds` serves both). NULL is a
   key value. A key with no columns (a reducer's `per ()`) is one class, every row in it.
3. Assign entity ids in first-seen order over every row via an
   `unordered_map<string, idx_t>`.
4. Fill a `row_to_entity` vector of size `num_rows`.

The result is an `EntityMapping { num_entities, row_to_entity }` on the
`SolverInput`, which is what makes several rows share one solver column.

---

## 4a. PHASE 1.6 — scope checks, and how a clause is read

`RunScopeChecks` runs `LogicalDecide::scope_checks` (stage 04) in order, before any
coefficient is evaluated, over the materialized rows — an empty input runs none. They are
the data checks of [`syntax_reference.md`](../../00_project_overview/syntax_reference.md)
§7.3:

| Kind | Fails when | Message |
|---|---|---|
| `KEEPS_A_ROW` | the `when` keeps no row | `DECIDE: when <c> matches no rows.` |
| `DETERMINES` | a class of the key (over the kept rows, and a reducer's own `when`) shows two values, NULL equal to NULL, or two instances of the decision | `DECIDE: per <K> does not determine <value>[ (<context>)]. <class> has more than one value: <a>, <b>.` · `... the decision <x> (declared ...). <class> covers more than one <x>.` |
| `READS_A_ROW` | a `by` group of the kept rows has no row the reducer's own `when` keeps | `DECIDE: <reducer> has no rows for <group>.` · `... has no rows.` for `by ()` |

A class or group is named by its key's values as SQL literals (`D.depotID = 'D1'`); over
`()` the message says "The kept rows". Groups come from `LookupOrBuildPerGroupIds` with
NULL a value. A key with no columns (`per ()`, `by ()`, a reducer's `per ()`) is one group
of every row the filter keeps: `BuildGroupIds` says so once, for every caller. A key that
is the same on every row (`by (1)`) hashes to a constant vector, which `BuildGroupIds`
flattens before each row reads its own hash. A check reads its `when` on every row
(`KeptRows`), a reducer's own `when` on the rows that keeps, and its key and value only on
the rows it looks at.

Every clause is then read by these rules; §5 gives the row maps they produce:

- **Kept rows first.** Its `when` is read before anything else, on every row, the parts
  of an AND or an OR left to right, each only on the rows the earlier parts leave
  undecided -- an AND's on the rows they keep, an OR's on the rows they do not
  (`KeptRows`; the composed rewrite folds the clause's `when` in front of a term's own).
  Anything else, `not` included, is computed whole. Nothing is then
  computed on a row the `when` drops: a reducer's own `when` is read on the kept rows,
  a term's coefficient on the kept rows its own `when` keeps, a bound, a `by` key and a
  class key on the kept rows (`ExecuteOnReadRows`, which slices each chunk to the rows an
  expression reads and leaves the others NULL; `LookupOrBuildPerGroupIds`' `read_rows`,
  which builds such a clause's groups afresh rather than from the cache's, computed on
  every row). `ExtractDoubleColumn` writes 0 for a row it does not read, so a NULL or a
  failing expression there is no error. An empty set has already failed a check (D4,
  D8), so formulation asserts rather than refuses one.
- **Grouping.** The aggregate shape groups the kept rows by its `by` key
  (`group_keys[constraint->group_key]`), NULL a value: a group's members are the rows a
  reducer's `when` keeps (all kept rows for `<>`, whose bound values are all excluded),
  its bound the tightest over all its kept rows. A row the reducer's own `per`
  de-duplication drops leaves the group when the clause is a hard MIN/MAX
  (`minmax_agg_type`): the members are the rows the reducer reads, and a zeroed row
  would be a member pinned at 0. The rows shape under `per K:` emits one row per class,
  at its first kept row; under `when` alone, one per kept row.
- **Bound-side reducers** are each evaluated over their own `by` groups of the kept rows
  (`EvaluateRhsReducerPerGroup`), whatever the clause's shape, and broadcast
  by each row's own group.
- **De-duplication** for a reducer's own `per` keeps the first row of each class among
  the rows its `when` keeps (`BuildQualifierKeepMask`'s `eligible`), within each group:
  a class whose first joined row the `when` drops is still counted.
- **The objective**: its `when` is folded into every
  term's filter, so reading, de-duplication and AVG denominators all follow it. A
  decision outside the reducers (`NONE`) keeps its coefficient on the first kept row only
  (`CountTupleTermsOnce`; a query-wide one moves to row 0, where stage 06 reads it), and
  so does a product of two query-wide decisions there (`CountOnce` on the evaluated
  bilinear term; one inside a reducer is SUM and keeps every row). The two-level form
  groups with NULL a class.
- **Composed clauses** read the rows their folded filter keeps and de-duplicate among
  them. Each takes its repair id here, after every prepared
  and absorbed clause (`ComposedConstraint::repair_group_id`), since formulation drops rows
  (a `<>` moved into indicator rows) and no count of the model's rows is the id space.
- **Classes of a grouped row** (D28, `CollectGroupClasses`, under DIAGNOSE only). The
  rows of a clause over `by` groups -- an aggregate-shape group row, an easy
  MIN/MAX's member rows, a composed clause's one row -- stand for every class of its
  `per` key in their group: with `per K:` in front (`per_key = NAMES_CLASSES`,
  `class_key`) the key's values among the group's kept rows (`LookupOrBuildPerGroupIds`
  over the kept rows, first-seen order); with no `per` (`NONE`) every kept row of the
  group; `per ()` the whole clause (one `WHOLE_CLAUSE` set its groups share). A per-row
  clause under `per K:` (`ONE_ROW_PER_CLASS`) calls it with its classes as the groups, so
  each class's row gets a set naming that class. One `SolverInput::ClassSet` per group with
  kept rows, its `Kind` saying which, referenced from
  `EvaluatedConstraint::group_class_set` (a composed clause's from its
  evaluated `class_set`, over the rows its `when` keeps); the model keeps the table and an
  expanded diagnosis reports the group once per class (`FanOutByClass`).
- **Helper decisions** (the value rewrite, `EvaluateReducerValueLinks`): each link's
  reducer is read on the rows the clause's `when` keeps and its own `when` keeps among
  them, grouped by its `by` key over the kept rows (`LookupOrBuildPerGroupIds`, NULL a
  value; the scope checks guarantee no group is empty); its coefficients come from
  `EvaluateTermColumns`, one scan for every term; its `per` de-duplicates within each
  group, a dropped row leaves the group, and AVG divides by the members left. The result
  is a `ReducerValueLinkSpec` carrying the members alone, with the link's label and
  provenance; stage 06 states the definition over them (an equality per group for
  SUM/AVG, an extremum link for MIN/MAX). `BuildSolverInput` frees a helper's floor before
  the rigid box is captured; a refusal in formulation names a helper by its reducer.
- **A NULL subquery** is named by its text as DuckDB prints it:
  `NullSourceContext::fragments`, set wherever a prepared clause, the objective or a
  helper's members are read, maps the flattened column back to the subquery's source
  fragment.

---

## 5. PHASE 2 — coefficient evaluation

### Expression transformation

`BoundColumnRefExpression` nodes reference columns by table binding;
`ExpressionExecutor` needs chunk positions. `TransformToChunkExpression` converts:

| Node | Becomes |
|---|---|
| `BoundColumnRefExpression` | `BoundReferenceExpression` on `binding.column_index` |
| `BoundFunctionExpression` | recurse into children, copy `bind_info` |
| `BoundCastExpression` | recurse into child, re-wrap with `AddCastToType` |
| `BoundAggregateExpression` | a reference to an extra chunk column holding that reducer's per-group value, cast back to the reducer's own type (see `EvaluateRhsReducerPerGroup`) |
| anything else | copied as-is |

A reducer reached with no substitution prepared — inside a `when` condition or a
coefficient, where it has no meaning — is rejected with `InvalidInputException`.

Results are cached per `Finalize` call, and the cache outlives every
`ExpressionExecutor` created below it, so executors may safely retain references
into cached entries.

**Precondition: resolution has already happened.** The `BoundColumnRefExpression`
branch uses `binding.column_index` as a chunk position, and the two indexings agree
only because `ColumnBindingResolver` already rewrote DECIDE's expressions. That
enumeration is maintained by hand — see
[`../03_logical_plan/done.md`](../03_logical_plan/done.md) §4.

### Per-term coefficients

Each term's coefficient expression is transformed, executed chunk by chunk against
`gstate.data`, and cast to `double` via `DefaultCastAs(LogicalType::DOUBLE)` —
**one conversion, on the result of the complete typed expression**, which is what
keeps a data cast's SQL semantics intact. Values accumulate into
`row_coefficients[term_idx]` indexed by global row.

Data-only LHS terms carry `INVALID_INDEX`. For aggregate constraints those fixed
coefficients are still evaluated per active row; the model builder subtracts their
row or group sum from the RHS.

### RHS

- **Constant** — extracted once and broadcast.
- **Expression** — evaluated per row, giving row-varying bounds.
- **Reducer** — evaluated per group and by reducer kind
  (`EvaluateRhsReducerPerGroup`).

The canonical RHS classification decides which: a bound tagged
`QUERY_WIDE_BOUND_TAG` is a shared scalar; anything with a row-varying component
stays data-backed.

### `when`, `by` and `per` → `row_group_ids`

One per-row vector assigns each row to a row of the clause, or excludes it. The clause's
`when` is read first (`KeptRows`); every map is built over the kept rows only, NULL a key
value, with ids dense in the order their rows first appear:

| Clause | `row_group_ids` | `num_groups` |
|---|---|---|
| no `when`, no reducer `when`, no `by`, no `per K:` over the rows shape | empty (fast path — all rows are one implicit group) | 0 |
| aggregate shape or easy MIN/MAX, `by (G)` | each row's `by` group, on the rows its reducers read (a kept row some reducer's own `when` keeps; every kept row for `<>`); `INVALID_INDEX` elsewhere | number of groups |
| aggregate shape or easy MIN/MAX, `by ()`, with a `when` or a reducer's own `when` | 0 on the rows its reducers read, `INVALID_INDEX` elsewhere | 1 |
| rows shape under `per K:` | the class id on each class's first kept row, `INVALID_INDEX` elsewhere | number of classes |
| rows shape with a `when` only | 0 on kept rows, `INVALID_INDEX` on dropped rows | 1 |

The bound side of the aggregate shape gets its own map with the same numbering over every
kept row (a bound-side reducer reads its own `by` groups, §4a). A `per K:` over the
aggregate shape or an easy MIN/MAX adds no row: it only names the classes each group's
row stands for (D28, §4a).

Keys use a `string`-keyed hash map with the same NULL-safe tagging as entity mapping.
The ids a key takes over every row are cached (`PerGroupCache`) across every constraint,
check and the objective, so one key is evaluated once however many clauses use it; a
clause whose `when` drops rows builds its groups afresh over its kept rows
(`LookupOrBuildPerGroupIds`' `read_rows`), since the cached key was computed on every row.

### A reducer's own `when`

Term-level filters are carried on individual terms, not on the whole constraint:

1. Each distinct filter expression is evaluated once into a boolean row mask, on the
   clause's kept rows only (`KeptRowsOfEach`).
2. Linear, bilinear and quadratic terms get coefficient `0` on rows their own
   filter excludes — **for that term only**.
3. A row joins a group when the clause's `when` keeps it and some reducer's own `when`
   keeps it (a reducer with no `when` keeps every kept row). A `by` group that one
   reducer's own `when` leaves with no row has already failed the scope checks (D8).

So one clause can carry independent filters:

```sql
sum(when active: x * a) by (grp) + sum(when priority: x * b) by (grp) <= 10
```

`active` filters only the `a` term and `priority` only the `b` term; each `grp` group is
one row, over the rows either keeps. A group where `priority` keeps no row is refused:
*"DECIDE: sum(when priority: x * b) has no rows for grp = 'b'."*

### A reducer's own `per`, and `SUM(D: e)`

`sum(per K: e)` asks for one contribution per **class** of `K`, and the colon form
`SUM(D: e)` is `sum(per D: e)` with `K` every column of `D`; a join repeats each class
once per matching row. De-duplication (`BuildQualifierKeepMask`) keeps the first row of
each class among the rows the reducer reads — the kept rows its own `when` keeps — and
zeroes the rest. That is sound because the scope checks have already proved that every
row of a class carries the same value (`DETERMINES`, *"per D does not determine ..."*), so
whichever row is kept gives the same answer. A class whose first joined row the `when`
drops is still counted once.

De-duplication runs **inside** each `by` group, after the clause's `when`, the reducer's
own `when` and the grouping. Group ids are dense in `[0, num_groups)` and class ids in
`[0, num_entities)`, so `(group, class)` flattens into one dense index with no hashing.
The mask is folded into the term's filter (`ApplyQualifierToFilter`), so AVG's
denominator counts classes. For a hard MIN/MAX, a row the de-duplication drops leaves the
group (§4a).

### AVG scaling

`AVG` was rewritten to `SUM` by the optimizer and tagged `AVG_REWRITE_TAG`. Terms
marked `avg_scale` are divided by the number of rows the reducer reads in the row's group
(`ScaleAvgRows`):

| Clause | Denominator |
|---|---|
| `by ()` | the kept rows (every row with no `when`) |
| `by (G)` | the kept rows of the row's `by` group |
| a reducer's own `when` | further, only the rows it keeps |
| a reducer's own `per` | further, one row per class (the de-duplication above) |

In the objective the two-level form's classes are the groups. The scope checks guarantee
no denominator is zero.

Quadratic AVG terms scale their inner linear coefficients by `1/√N`, so the
resulting outer product is scaled by `1/N`.

### Validation

Every coefficient and RHS value is checked after evaluation:

- **NULL** → `InvalidInputException` suggesting `COALESCE()` or a `WHERE` filter.
- **NaN / Infinity** → `InvalidInputException` naming the common causes (division
  by zero, arithmetic overflow, NULL propagation through math).

Both apply to constraint and objective coefficients alike.

**Infinity is admitted on one path only: the values that become a row's `rhs`.** An
infinite bound is not an error there — it is the absence of a constraint (`<= +inf`)
or one nothing satisfies (`>= +inf`) — and the model validator has always accepted a
non-finite `rhs` while rejecting NaN. A constant bound was never checked at all, so
the per-row expression path was the only one refusing what the rest of the pipeline
handled; canonicalization rebuilds `x + v <= K` as `x <= K - v`, which put the *same*
bound on the strict path purely because of how it was written.

Two values sit on that path, and both now follow the rule. The row's own bound is
one. The other is the input of a **bound-side reducer**: `min(x) by (g) <= max(cap) by (g)`
folds a column to one value per group, and `EvaluateRhsReducerPerGroup` extracted
that column strictly, so a single infinite row in `cap` refused the query outright —
before grouping, before folding, before anything knew which group the row belonged
to, and before the reducer's own `when` or `per` de-duplication had masked the
rows that contribute to nothing. The literal spelling of the same bound was accepted
and classified per group, so the outcome depended on how the bound was written.

It is now uniform: an infinite bound behaves the same in every spelling, and the
solver decides what it means. `inf - inf` is still NaN, and still refused — a
reducer's infinities reach the per-row extraction that reads the folded bound back,
where the NaN guard is unchanged, so admitting them upstream costs nothing.

**A non-finite bound is not absorbed into the column box.** `AbsorbVariableBounds`
(stage 05) folds a bare `x OP const` into `lower_bounds` / `upper_bounds`, each
initialized to a ±1e30 sentinel and tightened with `min` / `max`. Those sentinels swallow an infinity
in one direction and keep it in the other, so one spelling of one bound became "no
bound" and its mirror reached the bounds validator as a non-finite column — an
internal error, which also invalidates the connection for every later query in the
session. Absorption is an optimization, not a semantic step, so a non-finite value is
left unabsorbed and takes the ordinary constraint path: `x >= +inf` becomes one
infeasible row and `x <= +inf` one satisfied row, the same answer the rebuilt
spelling `x + v >= +inf` gives.

The exception is Big-M, which needs a constant that dominates the row's range, and
nothing finite dominates infinity. Every rewrite that faces this — `<>`, MIN/MAX,
ABS — classifies the bound before asking for an `M`, and all of that now lives at
stage 06 along with the rows it guards. See
[`../06_model_formulation/done.md`](../06_model_formulation/done.md) §9.

The one Big-M still derived here is the auto-`M` refill for `norm(e, 0)` links,
whose `M` bounds an expression rather than answering a comparison, so there is no
bound to classify. It calls `DecideTightPerRowBigM` across the stage boundary.

A *variable* with no finite bound is the separate case, and the answer there is also
refusal rather than a constant — there is no fixed Big-M anywhere in DeciDB. The
order in which this stage calls the linearizers is load-bearing for it: ABS runs
first, because pinning an ABS auxiliary is also what determines its column box, and
every linearizer after it derives its `M` from column boxes. See
[`../06_model_formulation/done.md`](../06_model_formulation/done.md) §9.

### The construct routing — a decision this stage reads, not one it makes

Whether a construct is lowered is a **formulation** choice, and formulation belongs to
stage 05. This stage reads the answer off the operator and applies it.

```cpp
const bool native_abs = use_native_constructs.abs;          // decided by stage 05
const NativeConstructPolicy native_min_max {use_native_constructs.min_max,
                                            force_native_constructs};
VarIndexer var_indexer = VarIndexer::Build(solver_input);   // the flat column space

DeriveAbsAuxiliaryBounds(solver_input, decide_var_names, /*refuse_when_unbounded=*/!native_abs);
if (native_abs) { EmitNativeAbs(solver_input, var_indexer); }
else            { LinearizeAbsMaximize(solver_input); }
LinearizeMinMaxConstraints(solver_input, var_indexer, decide_var_names, native_min_max);
LinearizeNotEqual(solver_input, var_indexer, decide_var_names);
LinearizeBilinear(solver_input, decide_var_names);
...
LowerDecideConstructs(solver_input, var_indexer, decide_var_names, use_native_constructs);
```

`use_native_constructs` is a `SolverConstructSupport` copied from
`LogicalDecide::use_native_constructs`, which stage 05 filled in from the chosen
backend before it rewrote anything. **This stage must not ask a backend what it
supports.** The routing has to be the same answer the rewrites upstream were selected
against, and a second, independently-derived answer is exactly how the plan and the
solve come to disagree about what was lowered. Deferring the choice also kept stage 05
from acting on it: see [`../05_optimizer/done.md`](../05_optimizer/done.md) §0.

**The `VarIndexer` is built before linearization, not after.** It reads `num_rows`, the
variable scopes and the entity mappings — all settled by the time the first pass runs —
and only the global block grows afterwards, with `total_vars` refreshed before the
solve. Building it first is what lets every construct site emit in flat columns at the
point it decides something, instead of stashing a decision for a second pass to
execute. Four such deferrals used to exist, one per construct, and they existed for no
reason but ordering.

**MIN/MAX arrives as a policy, not an answer**, and the difference matters. It reaches
this stage as a `NativeConstructPolicy` — the capability, plus whether a declared
construct is used everywhere or only as a fallback — and the shipping policy is
*fallback*: the lowering is the smaller model wherever it is valid, so native is
reserved for the clause that has no valid Big-M at all. Whether a given clause has one
is a question about **evaluated coefficients**, which nothing before this stage can
answer, so this stage answers it — per clause, from the reach of that clause's own
family. That is applying a decision to data, not making one; the distinction is the
same one the `<>` range collapse already relies on. One statement can have a bounded
clause and an unbounded one, and each gets the formulation it can actually use.

`DECIDB_NATIVE_CONSTRUCTS` A/B-tests the two arms on one machine: `off` forces every
construct down its lowering path, `force` states every declared one natively, and `on`
(the default, and what an unset variable means) is the shipping policy. For MIN/MAX the
A/B has to be `force` against `off`: on a bounded shape the default *is* the lowering,
so comparing it against `off` would compare the lowering with itself.

The backend itself reaches this stage as a **name**. `PlannedSolverBackend()` resolves
`solver_backend_name` through the registry at the two points that are about to solve —
the primary solve and each diagnostic re-solve — so nothing here holds a solver handle
it is not immediately using, and nothing here re-selects.

**Only two constructs still choose between two emitters**, and the count used to be
six. `<>` no longer chooses at all: it is emitted once, as conditional rows, and
`LowerDecideConstructs` turns those into Big-M rows where the backend cannot state a
condition. Every MIN/MAX — a constraint, a flat objective, a two-level objective, an
outer aggregate over groups or group sums, a composed term and a MIN/MAX helper of the
value rewrite — goes through one
`EmitExtremumLink`, which makes the choice once for all of them. What is left is ABS,
where the two formulations are genuinely different objects rather than projections of
one description: see [`../06_model_formulation/done.md`](../06_model_formulation/done.md)
§9a and [`../07_solver/done.md`](../07_solver/done.md) §2.

Only the *hard* directions need a choice at all. `MAX(e) <= K`, `MIN(e) >= K`,
`MINIMIZE MAX`, `MAXIMIZE MIN` are exact with a one-sided envelope plus outer pressure:
no Big-M, so nothing to replace.

Both ABS arms read the same stage-05 tag (`abs_aux_idx`, `abs_is_pos_bound`) — stage 05
tags rather than fully lowering, because the Big-M constants are functions of evaluated
data, and that tag *is* the native-construct record. Neither arm decides anything, which
is what keeps the two comparable.

**A member that is already a column is passed through, not copied.** A general
constraint relates columns, so a member expression normally has to be pinned to a fresh
one — but `MAX(x)`, `MIN(x)` and `ABS(x)` name a decision variable directly, and there
`t = x` was a column and an equality row per data row to restate a column that already
existed. Presolve cannot substitute a column a general constraint reads, so the copies
survived into the solve: measured on `MAXIMIZE MAX(x)` at 15K rows, 0.97s with the
copies against 0.29s without, for the same answer. Only an exact renaming qualifies —
one variable term, coefficient 1, no constant — since `MAX(2 * x)` or `ABS(x - target)`
is a genuine expression and still earns a column.

The native columns that remain are **boxed**, not free, wherever a range is derivable —
the same `AuxRange` walk that produces the Big-M constant. A free continuous column is a
measured performance cliff, and it is earned only by the one case that has no range at
all, which is precisely the query the native path exists to answer.

The box is derived **per row**, on both constructs. Each argument column stands for one
row's inner expression, so one row's reach is what bounds it; and because
`AuxRange::CoverRowSided` folds the two ends in separately, a row open on one side keeps
the side it can derive, and a row whose contributors are all bounded gets a real box
even when another row's are not.

### Output

A `SolverInput` carrying `num_rows`, `num_decide_vars`, `variable_types`
(reported as `BOOLEAN` wherever `is_boolean_var` is set), resolved
`lower_bounds` / `upper_bounds`, the `EvaluatedConstraint` vector, objective
coefficient and variable-index arrays, entity mappings, and `sense`.

When the objective is quadratic, the same evaluation runs on `squared_terms` **in
addition to** `terms`, not instead of: `EvaluateObjectiveTermList` is called once
per non-empty list, filling `objective_coefficients` / `objective_variable_indices`
for the linear half and `quadratic_inner_coefficients` /
`quadratic_variable_indices` for the quadratic half. Each term's filter — its own
`when` with the objective's folded in — applies to each list independently.

`CoefficientColumn` keeps this compact: a column that is uniform across rows — a
broadcast Big-M coefficient, a constant RHS, a McCormick constant — stores one
scalar rather than `vector<double>(num_rows, K)`. Reads via `Get(row)` are
branchless on the storage kind, and mutation lazily promotes Scalar → Dense. A
third kind, `SparseMasked`, stores a uniform value at a sorted index set with
every other row implicitly 0, built for the `<>` per-row indicator path where
every active row gets `-M` and every excluded row 0.

---

## 6. PHASE 3 — build and solve

Hands off to stage 06 ([`../06_model_formulation/done.md`](../06_model_formulation/done.md))
and stage 07 ([`../07_solver/done.md`](../07_solver/done.md)). The hand-off happens
early: `solver_input.constraints` is populated as soon as bounds are resolved, and
the linearization passes then work on it in place.

The MIN/MAX objective (flat and two-level) and the composed MIN/MAX clauses are
emitted by stage 06, not here. What stays is the evaluation they need — a composed
term's per-row coefficients, the query-wide factor on a reducer, its filter mask,
the constant RHS, and the objective's member rows. The evaluated terms travel to stage 06 as
`ComposedMinMaxTermData`; the objective's shape travels as `MinMaxObjectiveSpec`.

### Assembly and formulation are separate passes

PHASE 3 is two methods, split by *what each one reads* rather than by where it sits
in the sequence.

`BuildSolverInput` **assembles**. It moves the evaluated constraints, objective and
links off `gstate`, resolves the variable types and the column box (the absorbed
bounds, the `lower_bounds` sentinel, the rigid snapshot, then
`DecidePropagateImpliedBounds`), folds each reducer's own `per` de-duplication into
the objective's filter masks, applies AVG scaling, and builds the `VarIndexer`. It
runs exactly once per query, because it is the pass that consumes `gstate`.

`FormulateModel` **formulates**, against a `FormulationBox` handed to it: the auto-`M`
refill for L0 links, the ABS / MIN/MAX / `<>` / bilinear linearizers, the MIN/MAX and
composed-MIN/MAX encodings, and finally `LowerDecideConstructs` for whatever the
chosen backend cannot state natively. It touches no `ClientContext`, scans no data,
and evaluates no expression — it is a pure function of the evaluated clauses plus a
box.

That purity is the point. A Big-M or a McCormick envelope is sound only for the box
it was derived from, and infeasibility diagnosis re-opens that box (see "The elastic
box" above). Because formulation is separable from evaluation, the same pass can be
run a second time, on a copy of `SolverInput` against the widened box, to derive the
constants an elastic model needs — without re-scanning the data to recompute
coefficients that a widened bound cannot change. `FormulateElasticModel` is that second
caller; see "The elastic box" above.

Two boundaries carry the weight:

- **Implied-bound propagation belongs to assembly, not formulation.** It derives
  bounds *from rows*, and in an elastic model those rows are the loosenable ones, so
  re-running it would undo the re-opening. The auto-`M` refill sits on the other side
  of the line for the mirror-image reason: it derives a *constant* from the box, so it
  has to be re-derived whenever the box changes.
- **Columns are settled before formulation.** `VarIndexer::Build` runs in assembly and
  the tail is handed the result; only the global block grows as auxiliaries are
  appended, which a debug assertion checks. A second formulation therefore names
  exactly the same columns as the first, which is what keeps `var_labels` and
  solution readback valid across both.

A slow-solve checkpoint report runs when a solve exceeds its budget; see
[`slow_solves.md`](slow_solves.md).

---

## 7. Readback (`GetData`)

`DecideGlobalSourceState` holds a scan over `gstate.data` and a
`current_row_offset`. `MaxThreads()` returns 1, so the sequential mapping between
data rows and solution values is exact.

The source state also owns a `scan_chunk`, sized to `gstate.data.Types()` via
`InitializeScanChunk` — narrower than the operator's output chunk by exactly
`total_decide_vars`, since DECIDE columns are appended after the buffered input.
`GetData` scans into `scan_chunk`, then `Vector::Reference`s each scanned column
into the output chunk's matching leading column (zero-copy). Scanning directly
into the wider output chunk would violate `ColumnDataCollectionSegment::
ReadChunk`'s `chunk.ColumnCount() == column_ids.size()` contract.

Each call scans the next chunk, fills the DECIDE columns — the last
`total_decide_vars` columns of the output — and advances the offset. The mapping
is:

```
global_row   = current_row_offset + row_in_chunk
solution_idx = gstate.var_indexer.Get(decide_var_idx, global_row)
value        = ilp_solution[solution_idx]
```

A **per-row** decision (`DecideVarScope::ROW`) has its own column on each row. A
**keyed** decision (`per K: x`, `ENTITY`) has one column per class of `K`: every row of
a class resolves to it and receives the same value, which is the point of the key. A
**query-wide** decision (`per (): x`, `SCALAR`) is one column every row resolves to.

The readback indexer is the owning `VarIndexer::Build()` form, so it outlives the
`SolverInput`.

### Type-specific projection

Solvers return doubles, and a MILP may return `0.9999999` for an integer. The
branch is on the variable's **output `LogicalType`**, not on how it was declared:

| Output type | C++ type | Projection | Reached by |
|---|---|---|---|
| `INTEGER` | `int32_t` | `round(value)`, range-checked | `x(BOOL)` and INTEGER auxiliaries |
| `BIGINT` | `int64_t` | `round(value)`, range-checked | `x(INT)` |
| `BOOLEAN` | `bool` | `value >= 0.5` | optimizer-created BOOLEAN auxiliaries |
| `DOUBLE` | `double` | direct, no rounding | `x(REAL)` |
| default | `int64_t` | as BIGINT | — |

A declared `x(BOOL)` therefore comes back as `int32` `0`/`1`, not as SQL `true`/
`false`: `BOOL` is a *domain* carried by `is_boolean_var`, and the variable's
DuckDB-facing type stays `INTEGER` so it can appear in arithmetic. The `BOOLEAN`
row is reached only by auxiliaries the optimizer declares with that type
outright.

**The integer rows are checked, not cast** (`DecideSolutionToInteger`). A solved
value is not bounded by the column's type: the solver works in doubles, and the
bound that actually pins a variable may be a column read at execution or an
aggregate row that bounds no single variable. So no earlier layer can promise the
value fits, and an unchecked cast of an out-of-range double is undefined in C++ —
it saturates on the platforms we build, which would present the type's limit as
the answer and return a row violating the query's own constraints. This is the
one place that can know, so it refuses by name instead.

`x(INT)` is `BIGINT` for the same reason: a 32-bit result column would truncate
optima that are ordinary (`SUM(x) <= 5000000000`), and `BIGINT` is what DuckDB
itself returns for generated integers. The limit that remains is the double's
rather than the column's — past `2^53` a double no longer counts consecutively,
so the solver never distinguished the value and no wider integer type recovers
it. `x(BOOL)` stays `INTEGER`: a `0`/`1` domain cannot reach either limit.

Each output vector is `FLAT_VECTOR`, written through `FlatVector::GetData<T>()`.

### Auxiliary variables

`GetData` emits a column for **all** variables, user and auxiliary. The `Projection`
operator that `plan_decide.cpp` places above `PhysicalDecide` prunes the auxiliary
columns, so only user-declared variables reach the result. Auxiliaries occupy the
last `num_auxiliary_vars` slots, and their columns are constant NULL: no one reads
them, so no value is converted or range-checked (a helper of `sum(x * 1000000000)`
past int32 would otherwise be refused under its internal name).

---

## 7b. `DIAGNOSE`: reporting on the run instead of returning it

Two things change when the statement carried the prefix (`PhysicalDecide::diagnose`,
copied from `LogicalDecide::diagnose`):

1. **The engines are armed.** `FinalizeSolveResult` reads the flag once as
   `diagnosis_armed` and turns on the shared pre-solve work the engines need —
   unbounded-ray extraction, tolerating an inverted column box through Build, and
   retaining the built model for the elastic engine. An unprefixed query pays for none of
   it, which is the whole point of the prefix being the trigger.
2. **A failure stops being an error.** Each failing terminal calls the operator-local
   `report(...)` instead of throwing: it stashes the findings and sets
   `gstate.diagnosis_only`, so `Finalize` returns `READY` with no solution and `GetData`
   emits nothing. `PhysicalDecideDiagnose`, sitting directly above in the same plan,
   swallows those (absent) rows in its `Sink` and emits the findings in its `GetData`.
   Without the prefix every failure terminal still raises, and raises the *state only* —
   naming the clause means running the elastic solve, which is exactly what the user did
   not ask for.

A successful solve clears the stash, so a `DIAGNOSE` over a query that worked finds
nothing and reports one `feasible` finding. The handoff is statement-scoped by
construction: `TakeDecideDiagnostic` consumes it, and nothing outside the statement reads
it.

**What stays raised even under the prefix.** DIAGNOSE explains the outcome of a *solve*.
A query that fails before one happens — a semantic error, a bound that contradicts a
type's own domain (`x <= -1` on a non-negative decision, checked in the sink state's
constructor), a model class the host's solver refuses — still raises, with the precise
message that path already had. There is no solve outcome to report on.

The slow-solve path is deliberately outside all of this: see `slow_solves.md`.

## 8. Source map

| Concern | Location |
|---|---|
| Everything above | `src/execution/operator/decide/physical_decide.cpp` |
| Operator fields | `src/include/duckdb/execution/operator/decide/physical_decide.hpp` |
| `DecideTerm`, `DecideConstraint`, `DecideObjective` | `src/include/duckdb/planner/decide/decide_prepared_model.hpp` |
| The pass that fills them | `src/optimizer/decide/decide_linear_form.cpp` |
| `SolverInput`, `EvaluatedConstraint`, `CoefficientColumn` | `src/include/duckdb/decidb/formulation/solver_input.hpp` |
| Logical → physical, entity key indices, input column names | `src/execution/physical_plan/plan_decide.cpp` |
| Binding resolution shielding | `src/execution/column_binding_resolver.cpp` |
| The `DIAGNOSE` operator (sink the rows, emit the findings) | `src/execution/operator/decide/physical_decide_diagnose.cpp` |
| `per`, `when` and `by` semantics; the scope test suites | [`../../03_expressivity/scope/done.md`](../../03_expressivity/scope/done.md), [`../../04_testing/scope/done.md`](../../04_testing/scope/done.md) |
| Its plan-generator case | `src/execution/physical_plan/plan_decide_diagnose.cpp` |
| Slow solves: report, continuation, Ctrl-C | [`slow_solves.md`](slow_solves.md) |
