# Scopes — `per`, `when`, `by`

Three keywords, three jobs:

| Keyword | Where | Job |
|---|---|---|
| `per K` | in front of a declaration, a constraint or a reducer's argument | how many decisions, constraints or reducer inputs exist: one per class of `K` |
| `when c` | in front of a constraint, the objective or a reducer's argument | which rows are looked at: those where `c` is true |
| `by (G)` | after a reducer | which rows that one reducer reads: the kept rows sharing the current value of `G` |

`per` never selects the rows a reducer reads, and `by` never changes how many
constraints exist. A *class* is the set of rows with one value of a key; a *kept row* is
a result row (after joins and `WHERE`) that the enclosing `when` keeps.

**Syntax**: `../../00_project_overview/syntax_reference.md` (SR below) §1.1 (the forms and their
defaults), §2.3 (keyed declarations), §4 (objective), §5.1–§5.5 (reducers, nesting), §6
(scoped constraints), §7 (variants, NULL, empty sets), §10.3 (the spellings that
are refused). This doc covers what each form means beyond the syntax, how the layers
build it, and where the code is.

---

## 1. Defaults

Every clause is a scoped clause; an omitted part takes its default.

| Omitted | Means |
|---|---|
| `per` on a declaration, a constraint or inside a reducer | `per row` — one per (kept) row; `per row:` may be written out |
| `per` on the objective | `per ()` — produced once; any other `per` is a parse error |
| `by` after a reducer | `by ()` — every kept row |
| `when` | every row |

So `sum(x) <= cap` is `per row: sum(x) by () <= cap`: one constraint per row, each
pairing the same total with that row's `cap`, which is the total against the tightest
`cap`. Under `per row` each class is one row, so it has one variant (§5).

## 2. Declarations — `per K: x(TYPE)`

- `x(TYPE)` is one decision per result row; `per K: x(TYPE)` one per class of `K`
  among the result rows (*keyed*); `per (): x(TYPE)` one for the whole query
  (*query-wide*). A key value with no result row has no decision; NULL is a key value,
  so NULL-keyed rows share one decision (D5).
- The key lists columns and relations of the `FROM` clause; a relation stands for all its
  stored columns (its generated columns are left out; `rowid` or a generated column named
  in a key is refused). The key is a
  set, so `per a, b:` and `per b, a:` share one key scope, and it is exactly what is
  written: no column leaves it because the clause also reads it.
- Each declarator has its own key (D7): `per D: open(BOOL), ship(INT)` leaves `ship`
  one per row.
- A decision is referenced by its bare name everywhere. `D.open` for a decision `open`
  is refused at bind time (*"DECIDE: write open, not D.open; a decision is referenced
  by its bare name"*), and in a `when`, a key or a `by` with *"DECIDE: D.open is a
  decision; when, per and by may only use data"*. The check runs only when `D` is a
  relation of the `FROM` clause and `D.open` names none of its columns, so a struct
  field or a real column `D.open` is unaffected (D43). It covers the constraints, the
  objective, the select list and `ORDER BY`.
- Every result row shows its class's value.

`DecideVarScope` (`common/enums/decide.hpp`) records the three cases as `ROW`, `ENTITY`
(keyed) and `SCALAR` (query-wide); a keyed declaration's `DecideVarScopeInfo` also keeps
the key as written (`declared_key`) for messages. Keys resolve in `FindOrCreateKeyScope`
(`decide_binder.cpp`), which finds or appends the `EntityScopeInfo` entry of
`entity_scopes` for that key; declarations and reducers with equal keys share it.
At execution `BuildEntityMappings` gives every row its class's solver column.

## 3. Constraints — `[when c] [per K]: C`

A scoped constraint is evaluated in four steps (SR §6.1):

1. **Filter.** Keep the rows where `c` is true. NULL counts as false.
2. **Generate.** Split the kept rows into classes by `K`; the body is produced once
   per variant of each class (§5). `per ()` is one class; no `per` is one class per kept
   row.
3. **Aggregate.** A reducer `agg(...) by (G)` reads every kept row whose `G` equals
   the variant's `G`, including rows outside the class.

```sql
per D.depotID: sum(ship) by (D.depotID) <= D.capacity          -- one per depot, over its rows
when S.priority per D.depotID: sum(ship) by (D.depotID) <= 3   -- depots with a priority shipment
ship <= 0.60 * sum(ship)                                       -- one per row, over all rows
ship <= D.share * sum(ship) by (D.depotID)                     -- one per row, over its depot's rows
```

- **`when` reads only the rows it needs.** A condition is any boolean expression over
  data; an `and` before `per` or `:` belongs to it, so no parentheses are needed. It
  takes no decision, no reducer and no window function. The parts of an `and` or an
  `or`, at any depth outside a `not`, are read left to right, each only on the rows the
  parts before it leave undecided, and nothing else of the clause — its terms, keys,
  bounds, checks, or the rows a rewrite adds for it — is computed on a row the `when`
  drops: `when w > 0: x <= ln(w)` takes no logarithm of 0, and a NULL on a dropped row
  is no error.
- **A `when` that keeps no row is an error** (D4), with or without a reducer, on a
  constraint or the objective: *"DECIDE: when S.status = 'actve' matches no rows."*
  An input with no rows at all is not an error; the result is empty.
- **Dropped rows keep their decisions**; the clause does not constrain them.
- **`per` does not group reducers** (D6). `per g: sum(x) <= 10` bounds the total over
  every kept row, once per class; the copies are one model row. To bound each group's
  own total, write `per g: sum(x) by (g) <= 10`.
- **NULL is a class** (D5). To leave NULL-keyed rows out, write
  `when g is not null per g:`.
- **One scope, one constraint.** In `per g: A AND B`, only `A` is scoped. A `BETWEEN`
  is one constraint, so both halves take the scope.
- **The bound.** With no `per`, a bound that varies inside a `by` group reduces to the
  tightest value of the group (MIN for `<=`/`<`, MAX for `>=`/`>`); `=` refuses a group
  whose bounds differ, and `<>` keeps every value as its own exclusion
  (`ReduceAggregateRhsPerGroup`, `physical_decide.cpp`; see
  `../sql_functions/done.md` → "Reducers as a Bound"). Under an explicit `per K:` each
  value it takes in a class is a variant, which bounds the group alike.

### How a constraint becomes model rows

`DecideCanonicalizer::ClassifyScoped` (`decide_canonicalizer.cpp`) reads the decision
side once and picks one of three shapes (SR §6.4):

| Shape | When | Model rows |
|---|---|---|
| ROWS | no decision-bearing reducer | one row per variant of each class, from its first kept row (§5) |
| AGGREGATE | decision reducers that share one `by` key, under whole-query factors, with only query-wide terms beside them | one row per `by` group of the kept rows, under the group's tightest bound |
| VALUE | anything else: a per-row or keyed decision beside a reducer, a factor that varies by row, reducers with different `by` keys, a reducer inside another expression, a decision reducer holding a reducer (§4), a product of decisions beside a reducer, or a `MIN`/`MAX` beside another term (or under a factor of unknown sign) that has a `by`, a non-constant bound or a query-wide decision beside it | each decision reducer becomes a helper decision with one instance per `by` group -- a nested one per path group, innermost first -- defined once; then one row per class |

The canonicalizer stamps a VALUE comparison with `VALUE_REWRITE_TAG`;
`DecideOptimizer::RewriteScopedReducers` (`decide_rewrite_scoped.cpp`) replaces each
decision reducer under it with a helper decision and records it in
`LogicalDecide::reducer_value_links`; stage 06 (`linearization_reducer_value.cpp`)
boxes each helper by its members' reach and emits its definition — an equality row per
group for `SUM`/`AVG`, an extremum link per group for `MIN`/`MAX`. Deck example 1,
`ship <= 0.20 * sum(ship)`, costs N rows of two entries plus one definition row, not N
rows of N entries. The helper is whole-numbered exactly when the reducer's value is
(`DecideValueIsWholeNumbered`, shared with the binder's strict-comparison gate), so
`<`, `>` and `<>` over a fractional helper are refused before the query runs, with the
binder's message (*"Comparison '>' is not supported here: 'sum(ship + 0.5) by
(D.region)' can take fractional values in this constraint, ..."*). A product or square
of decisions inside a reducer is accepted only in the AGGREGATE shape. `AVG` under `<>`
beside another term or with its own `when` or `per` is refused (`ClassifyScoped`'s
UNSUPPORTED shape). A `<>` over a square or a product of decisions left after the
bilinear rewrite (a product with a `BOOL` decision is made linear first) is refused in
stage 05 (`FindNotEqualConstraints`, *"<> over a product or a power of decisions is not
supported yet"*): the two rows a `<>` becomes hold a linear left side only.

- **A factor on a reducer** may be any decision-free term in a constraint:
  `D.share * sum(ship) by (D.depotID)` and a correlated subquery as the factor take the
  VALUE shape, and under `per` each value of the factor is a variant. In the objective a
  row-varying factor is refused (*"DECIDE objective: 'w' varies per row, so it cannot
  multiply SUM(x). ..."*). A decision as the factor (`s * sum(x)`) is refused in both
  (`PeelScale`: *"'s' is a decision, so it cannot multiply SUM(x). ..."*).
- **An easy `max(x) <= K` / `min(x) >= K`** with no `when` and no reducer filter is a
  bound on every row it reads, absorbed into the decision's box
  (`TryMatchEasyExtremumTarget`, `decide_bound_absorption.cpp`, D41), so a negative `K`
  makes `x` signed — in any spelling whose other side is a constant known before the
  query runs (`min(x) - 1 >= K - 1`, `2 * min(x) >= 2K`, `-min(x) <= -K`), exactly as
  for `x >= K`. With a `by`, each member is bounded by its group's bound; the
  reducer stays in the tree, so EXPLAIN and DIAGNOSE quote the clause as written.

## 4. Reducers — `agg([when c] [per K]: e) by (G)`

- **`by (G)`** reads every kept row whose `G` equals the current row's or class's.
  `G` lists columns, relations (all their stored columns; a column of the same name
  wins) and data expressions; NULL is one group; a key with one value on every row
  (`by (1)`) is `by ()`. A key written twice in one clause is one key, a subquery in it
  included (keys compare as sets, and a subquery by its text: `FindOrCreateGroupKey`).
  `by` follows `SUM`, `AVG`, `MIN`, `MAX`, `count(*)` and `norm(e, p)` (D34, D35), on
  either side; a data-only reducer on the bound side is evaluated over its own groups
  (`sum(x * v) by (grp) <= sum(v) by (grp)`).
- **Order inside a reducer**: the rows `by` selects, then the reducer's own `when`,
  then one value per variant of each class of its own `per` (each read at its first row
  the `when` keeps, §5), then the aggregate. The inner `when` filters this reducer only;
  a `when` in front of the clause applies first.
- **`AVG`** divides by the number of values it receives: kept rows, or variants under an
  inner `per`. **`MIN`/`MAX`** are unaffected by an inner `per`.
- **A `by` group that the reducer's own `when` empties is an error** (D8), naming the
  group: *"DECIDE: sum(when S.priority: ship) has no rows for D.depotID = 'D2'."*, or
  *"... has no rows."* under `by ()`. To leave such groups out, move the condition in
  front of the constraint.
- **The colon form** `SUM(D: e)` / `SUM(D, T: e)` means `sum(per D: e)` /
  `sum(per D, T: e)`: the same classes and variants, and it may take a `by`. It
  is echoed as written (D38). Its semantics and implementation are in
  `../decide/done.md` → "The colon form".
- **A reducer inside a reducer** nests to any depth, with `when`, `per` and `by` at every
  level, in a constraint and in the objective (SR §5.5). The **path rule**: a nested
  reducer reads, at the row it stands for, the kept rows every enclosing reducer's own
  `when` keeps that agree with that row on every enclosing `by` key and its own -- its
  path key. So `sum(max(x) by (g)) by (h)` takes one maximum per `(h, g)` pair, and a
  `when` emptying a path group is D8 naming the group by its path key (*"DECIDE:
  max(when c: x) has no rows for t.g = 'a', t.h = 'q'."*). An enclosing reducer receives
  one value per row it reads, or per class and variant of its own `per`; `AVG` divides
  by those. Over decisions every level is a helper decision keyed by its path (§3, "How
  a constraint becomes model rows"); over data alone a nest is a value on the bound side.
  Refused (D74): `norm` inside a reducer, a square or a product of decisions inside
  nested reducers, a reducer over data alone added inside one over decisions.

## 5. Variant signatures

A `per` key splits the kept rows into classes, and each class is one instance per
**variant** (D64): per distinct value of its **signature**, the tuple of what the body
reads once per class (SR §7.1; deck pp. 34–40):

- every tuple term, by value — a column or expression outside reducers, the bound and
  any factor on a reducer included; the data terms of one additive context, read
  through `+`, `-` and constant factors, are one value (D77), so written associativity
  never changes the count;
- every decision outside reducers, by instance — its row if it is per row, its class
  if it is keyed; a query-wide decision splits nothing;
- every reducer's `by` key, by value.
- every frame (`at(...) over (...)`, a range `agg(from ... to ...: e) over (...)`, SR
  §5.6), by its keys, never by the data it reads (D76): its timeline, and its peer when a
  position is relative to the anchor; plus its fallback where the fallback is reached. A
  first/last frame whose `within` columns the key holds is covered and splits nothing
  (`../../09_anr_language/done.md`, phases 4 and 5). Absence is no field: inside a
  reducer a frame that selects nothing drops that member, and a reducer left with none
  is absent -- its instance skipped, the member reading it dropped, the objective an
  error (D81, D82; phase 6).

An inner `per` key, and the `D` of the colon form, hand their reducer one value per
variant of each of its classes the same way (D65). Where the key fixes every field —
what the determination rule used to demand — each class has one variant and the model
is unchanged (deck p. 39). NULL is one value. The objective has one class, which must
hold one variant (D66):

```
DECIDE objective: 'w' takes more than one value over the kept rows (3, 5), so the objective would be produced more than once; move it inside a reducer or fix it with when.
DECIDE objective: the decision 'x' (declared per row) has more than one instance over the kept rows, so ...
DECIDE objective: 'g' in by (g) takes more than one value over the kept rows ('A', 'B'), so ...
```

**The trap (D65).** `sum(per D: D.opening_cost + T.unit_cost * ship)` over a depot with
two routes reads the depot twice, once per route: the per-row `ship` and the route's
`unit_cost` make each route a variant. `EXPLAIN ANALYZE` prints, under the clause, how
many values the reducer read from how many classes, and how many constraints a `per`
clause generated, wherever a class held more than one variant (SR §9).

**How the layers build it.** The canonicalizer reads each signature off the canonical
tree, once, right after `CanonicalizeTree` and `CanonicalizeObjective`
(`BuildConstraintSignatures`, `BuildObjectiveSignatures`, `CollectSignatureFields`,
`decide_canonicalizer.cpp`), into `LogicalDecide::signatures` (a `DecideSignature`: the
rows it reads, its key, its fields `VALUE | DECISION | BY_KEY`): a `per K:` clause's on
its wrapper (`SIGNATURE_TAG_PREFIX`), a qualified reducer's through a scope of its own
(`EntityScopeInfo::signature`, its tag repointed), the objective's as
`LogicalDecide::objective_signature`. A field the key already holds — a column of the
key, a decision keyed on its columns, a `by` key inside it — is left out, and an
expression with no field left has no signature. Execution numbers the kept rows by
variant (`BuildSignatureIds`, `physical_decide.cpp`): the rows shape emits one row per
variant at its first row; a qualified reducer de-duplicates by its variant scope's
entities, so every consumer of the qualifier reads one value per variant unchanged; the
objective's check stops the query on a second variant (`RunScopeChecks`). The aggregate
shape and the easy MIN/MAX already emit one row per `by` group under its tightest bound,
which covers every variant of the group, so their signature only names classes.

The remaining checks are `DecideScopeCheck`s of three kinds — `KEEPS_A_ROW` (D4),
`READS_A_ROW` (D8) and `FRAME_PARTITION` (D69: a `per` that reads a frame directly must
determine the frame's `within` columns, built by the canonicalizer's `PlaceFrames`) —
built on the bound tree (`BuildConstraintScopeChecks`,
`BuildClauseScopeChecks`, `BuildReducerScopeChecks`, `BuildObjectiveScopeChecks`),
carried on `LogicalDecide::scope_checks` and run by `PhysicalDecide::RunScopeChecks`
before any clause is evaluated; the objective's variant check runs with them. They also
run under `DIAGNOSE`.

## 6. Objective — `MAXIMIZE|MINIMIZE [when c] [per ()]: E`

- **Produced once.** `when c` filters the rows for the whole objective; one that keeps
  no row is an error (D4).
- **A decision outside the reducers** is legal when it has one instance over the kept
  rows, and counts once: `MAXIMIZE when id = 3: x` and `+ s` for a `per ()` decision,
  but not beside a `MIN` or `MAX` (*"DECIDE: 's' beside MIN or MAX is not supported
  yet"*). `MAXIMIZE x` over several rows would be produced more than once (§5, D66):
  *"DECIDE objective: the decision 'x' (declared per row) has more than one instance
  over the kept rows, so the objective would be produced more than once; ..."*.
- **A top-level reducer's `by`** must be one group over the kept rows:
  `MAXIMIZE when g = 'B': sum(x) by (g)` runs, `MAXIMIZE sum(x) by (g)` over two
  values of `g` is refused.
- **Nested reducers** (D2, D75), `outer(per k: inner(e) by (k))` the common case:
  `inner` once per class of `k`, NULL a class, then `outer` over those values -- any of
  `SUM`, `AVG`, `MIN`, `MAX` at either level, `when` and `per` inside either, any `by`,
  any depth, beside another term or under a factor, as in a constraint (§4).

  ```sql
  MINIMIZE max(per D.depotID: sum(ship) by (D.depotID))       -- the busiest depot
  MINIMIZE sum(per grp: avg(power(x - target, 2)) by (grp))   -- each group's mean square
  ```

  The canonicalizer tags each objective reducer over decisions that holds a reducer
  (`TagNestedObjectiveReducers`); the value rewrite makes it and every reducer inside it
  a helper decision, pushed the way the sense pushes the objective, and the objective
  reads the outer helper once. `sum([when c] per K: inner(e) by (G))`, `inner` a SUM or
  AVG with every column of `K` in `G`, is written as the objective's own sum instead
  (`InlineSumOfGroupSums`): each `G` group once, an inner AVG dividing by its group's
  size (`DecideTerm::avg_group_key`), so a square in `e` stays a convex QP. A `MIN` or
  `MAX` of a square inside nested reducers is refused.

## 7. `when` is a row filter, not `CASE`

`when` decides whether a clause applies to a row. SQL's `CASE WHEN` produces a value,
and is refused inside a DECIDE constraint or objective with one message wherever it
sits (`DecideCaseUnsupportedMessage`, `decide_binder.cpp`), which names the scopes and
the CTE alternative. When coefficients or bounds differ by condition, compute them as a
column first:

```sql
WITH effective AS (
  SELECT E.empID, P.projectID, P.hours,
         CASE WHEN title = 'Director' THEN 2 ELSE 1 END AS effectiveness
  FROM Employees E JOIN WeeklyPlan P ON E.empID = P.empID
)
SELECT * FROM effective
DECIDE new_hours(INT)
SUCH THAT per projectID: sum(new_hours * effectiveness) by (projectID) >= 60
MINIMIZE sum(new_hours)
```

This is one weighted sum per project, not the two independent constraints
`per projectID: sum(new_hours) by (projectID) >= 60 AND when title = 'Director' per
projectID: sum(new_hours) by (projectID) >= 30`. A `CASE` evaluates top to bottom and
returns the first match, so overlapping conditions keep their priority, which separate
`when` clauses cannot express.

## 8. Composing with arithmetic

A filtered or grouped reducer is one term to the canonicalizer, which never opens it:
`sum(when w: x) + 3 <= 10`, `2 * sum(when c: x) <= 10`, `sum(when c: x) / 2 <= 5` and
`sum(when w: x) + (sum(x) + 3) <= 10` all run. Constant offsets move to the bound; a
factor is peeled outward onto the term and stays outside it to the solver row, which
keeps `MIN`/`MAX` exact under a negative factor
(`../../01_pipeline/04_canonicalizer/done.md`). In the objective, additive constants
(`MAXIMIZE 2 * sum(when c: x) + 3`) accumulate on
`LogicalDecide::objective_constant_offset`.

## 9. Echo

EXPLAIN, DIAGNOSE and the scope messages print a clause in the plan style: lower-case
reducers and keywords, the `when c per K:` qualifier in front, a reducer's `by (G)`
after it, and the colon form as written; the defaults `per row:` and `by ()` are not
printed (D32). A key prints as written (`per D`, `per D.depotID`). See
`../explain/done.md` §2. DIAGNOSE's `group` column holds the `per` key value of an
expanded finding, a NULL key as the text NULL; its `scope` column says `entity` for a
keyed decision (`../diagnose/done.md`).

## 10. Implementation by layer

**Parser.**
- The lexer filter `base_yylex` (`third_party/libpg_query/src_backend_parser_parser.cpp`)
  makes the words keywords inside a DECIDE clause only: `WHEN` becomes `WHEN_DECIDE`
  (`WHEN_DECIDE_OBJECTIVE` after `MAXIMIZE`/`MINIMIZE`), `PER` becomes `PER_DECIDE`,
  `PER_DECIDE_PAREN` before `(` or `PER_DECIDE_ROW` before `ROW`, and a `BY` directly
  after `)` becomes `BY_DECIDE`. A `WHEN` inside `CASE … END` (`decide_case_depth`)
  stays SQL's, so the binder can refuse the `CASE` by name. The lexer state is pushed
  at every `DECIDE` and `SUCH` and popped when that clause reduces
  (`PGDecidePushLexState` / `PGDecidePopLexState`), so a nested DECIDE leaves the outer
  clause's keywords intact (`../../01_pipeline/01_parser/done.md`).
- Grammar (`third_party/libpg_query/grammar/statements/select.y`): `decide_when`,
  `decide_per`, `decide_scope`, `decide_by`; a scope in front of a constraint or the
  objective (`decide_scope ':' decide_item_expr`), of a declaration
  (`decide_per ':' ColId '(' variable_type ')'`) and inside a reducer
  (`func_name '(' decide_scope ':' func_arg_list ')' [decide_by]`).
  `applyDecideScope` builds the parsed markers `SCOPE_WHEN_TAG`, `SCOPE_PER_TAG`
  (children: body, then key) and `SCOPE_PER_ROW_TAG`; `by` is `REDUCER_BY_TAG`. Parse
  errors: a parenthesized key, an expression in a key (`makeDecideScope`), a `per`
  other than `()` on the objective. `%expect 4`.
- The refused spellings keep error-only productions: after a constraint, the objective
  or a reducer they build the `PG_AEXPR_WHEN_CONSTRAINT` / `PG_AEXPR_PER_CONSTRAINT`
  markers (the objective's condition through `WHEN_DECIDE_OBJECTIVE` and
  `decide_objective_when_condition`), and the refused declaration spellings raise in the
  grammar. `RefuseRemovedDecideSpellings`
  (`src/parser/decide/decide_removed_spellings.cpp`) throws the message naming the new
  spelling, built from the user's own expressions; it runs from `TransformSelectNode`
  and, for a parse tree stored in a view or macro, at the top of `BindDeclarations`
  (D54). Messages: SR §10.3.

**Binder** (`src/planner/expression_binder/decide/`).
- `ValidateDecideScopeSpellings` (`decide_binder.cpp`) checks where a scope and a `by`
  may stand and that conditions and keys use data only;
  `RefuseQualifiedDecisionReferences` (`decide_declarations_binder.cpp`) refuses `D.x`.
- `DecideConstraintsBinder::BindScopedConstraint` binds the body, then wraps it: a
  `when` as a conjunction tagged `WHEN_CONSTRAINT_TAG` (child 0 the constraint, child 1
  the condition), a `per` as one tagged `PER_CLASS_TAG` (child 0 the constraint,
  children 1.. the key columns, none for `per ()`), with the key's written text
  recorded as a source fragment.
- `DecideBinder::BindScopedReducer` puts the inner `when` on
  `BoundAggregateExpression::filter` and tags an inner `per` with the key scope's index
  (`MakeQualifiedReducerTag`); `BindReducerBy` registers the key in
  `LogicalDecide::group_keys` and tags the reducer `MakeGroupKeyTag`;
  `BindQualifiedReducer` binds the colon form onto the same key scope and adds
  `COLON_FORM_TAG`.
- `DecideObjectiveBinder::BindScopedObjective` wraps the objective's `when` the same
  way; a nested reducer binds as nested aggregates, anywhere (§4, §6).

**Canonicalizer** (`src/planner/decide/decide_canonicalizer.cpp`): `ClassifyScoped`
(the shape; a decision reducer holding a reducer is VALUE), `PeelScale` (factors), the
objective's nest tags, and the scope checks and signatures (§5), a nested reducer's
along its path (`DecideReducerPath`, `decide_reducer_path.hpp`).

**Optimizer** (`src/optimizer/decide/`): `RewriteScopedReducers` (the VALUE shape, a
nest innermost first keyed by its path, the nested objective), `TryMatchEasyExtremumTarget`
(D41).

**Execution** (`src/execution/operator/decide/physical_decide.cpp`).
- `RunScopeChecks` first; then each clause reads only its kept rows: `KeptRows` /
  `KeepRowsWhere` evaluate a condition (the `and`/`or` order above), and
  `ExecuteOnReadRows` runs every other expression on the rows a mask marks, NULL on
  the rest.
- `BuildGroupIds` partitions rows by a key (NULL a class) and `LookupOrBuildPerGroupIds`
  caches one partition per key, so every clause and check on the same key shares the
  numbering. `CollectGroupClasses` lists the classes a model row stands for.
- The prepared model (`decide_prepared_model.hpp`) carries the scope on
  `DecideConstraint` — `when_condition`, `group_key` (the shared `by` key of the
  AGGREGATE shape), `per_key` (`PerKeyRole`: `NONE`, `ONE_ROW_PER_CLASS`,
  `NAMES_CLASSES`) and `class_key` — and on `DecideObjective` — `when_condition`, and an
  AVG term's `avg_group_key`.
- `BuildQualifierKeepMask` keeps one row per class of an inner `per` (or colon form) in
  each group; `ReduceAggregateRhsPerGroup` reduces a varying bound per group.

**Rendering** (`src/planner/decide/decide_source_provenance.cpp`):
`CollectDecideExpressionStrings` walks the wrappers, building the front qualifier from
`WhenQualifier` and `PerClassQualifier` (`FrontQualifier`, `AttachDecideQualifier`);
`RenderReducer` prints a reducer's inner scope and `by`, a nested one nested.

## 11. Scale

A clause emits one row per class (ROWS, VALUE) or per `by` group (AGGREGATE), so a
high-cardinality key multiplies the model. The optimizer's work on large models —
constraint-to-bound conversion, skyband pruning, drop-solve-validate-refine — is in
`../../01_pipeline/05_optimizer/done.md`.

## 12. Tests

The scope suites, the `_scope_oracle.py` reference model and the brute-force
enumerator are described in `../../04_testing/scope/done.md`:
`test/decide/tests/test_scope_{bare_clauses,combinations,deck_examples,declare,explain_diagnose,generation,objective,reducer_by,reducer_inner,removed_syntax,value_rewrite,variants}.py`, `test_anr_variants.py`, `test_anr_composition.py`, `test_anr_diagnostics.py`,
with `test/decidb/test_scope_parser.test` for the parser cases.
`test_no_old_spellings_remain.py` keeps the refused spellings out of every test surface
except `test_scope_removed_syntax.py`.
