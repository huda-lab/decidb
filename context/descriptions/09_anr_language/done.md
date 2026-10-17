# ANR language surface — implemented

Items 1 to 4 of the plan have shipped: **the new spellings parse**, **a declaration's
`per` runs**, and **a scope runs in front of a constraint or the objective, after a
reducer (`by`) and inside one**. What the engine accepts is recorded in
[`../00_project_overview/syntax_reference.md`](../00_project_overview/syntax_reference.md);
what the extension will do is in
[`../00_project_overview/anr_language_extension.md`](../00_project_overview/anr_language_extension.md),
and the remaining items are in [`todo.md`](todo.md).

## Item 1 — new spellings parse

- The grammar accepts `PER key: x(TYPE)` in a declaration, `[WHEN condition] [PER key]:`
  in front of a constraint, an objective or a reducer's first argument, and `BY (key)`
  after a reducer (plain call, scoped call, or the colon form `SUM(D: e)`). Statements
  print back in these spellings and re-parse to an equal statement.
- Inside a DECIDE clause, subqueries written there included, `WHEN`, `PER`, a `BY`
  directly after `)` and a `ROW` directly after `PER` are keywords; a column with such
  a name is written quoted there, so the old `C PER row` is now `C PER "row"`. The
  lexer emits `PER_DECIDE`, `PER_DECIDE_PAREN`, `PER_DECIDE_ROW` and `BY_DECIDE` only
  inside the clause, so ordinary SQL — including `GROUP BY` in a subquery written
  inside the clause — is untouched. Details:
  [`../01_pipeline/01_parser/done.md`](../01_pipeline/01_parser/done.md).
- Three parse errors are exact: `PER` other than `()` on an objective, a parenthesized
  key, and an expression in a `PER` key (messages in the behaviour file, §9).
- `PER ROW` is the default written out. It first left no node; since D19 (2026-10-17) it
  builds `PG_AEXPR_SCOPE_PER_ROW`, so a clause written with it follows the ANR rules.
- Every other scope answered *"DECIDE: 'PER key:' is not supported yet"* (or `PER ():`,
  `WHEN condition:`, `BY (key)`) until items 2–4 bound it; the gate, now part of
  `ValidateDecideScopeSpellings`, keeps that answer inside ordinary SQL subqueries
  written in the clause.
- The grammar's conflict budget fell from `%expect 6` to `%expect 4`: the two conflicts
  the postfix-operator states carried were on the token `PER` being readable as an
  identifier, which it no longer is inside the clause.
- Tests: `test/decidb/test_scope_parser.test` (accept, exact errors, boundaries, old
  spellings and plain SQL through the engine) and the scope case in
  `test/common/test_decidb_plan_serialization.cpp` (round trips including the deck
  examples 1–8 in both forms, a scope covering exactly one constraint, `PER ROW`
  equivalence, parse errors at the parser).

## Item 2 — `decide per K: x(TYPE)`

- A declaration's key binds in `DecideDeclarationsBinder::BindDeclarations`, which no
  longer runs the "not supported yet" gate on declarations. `per ()` is the query-wide
  scope of `scalar x`; no `per` (or `per row`) is per row; `per K:` gets a key scope.
- `FindOrCreateKeyScope` (`decide_binder.cpp`) resolves each key element against the
  query's own `FROM` clause, never an outer query, spelled as SQL spells it (`c`, `R.c`,
  `S.R.c`; `R`, `S.R`): a declared decision, by its name or `T.x` spelling, is the spec's
  §9 error; then a column (an unqualified `USING` column means its primary relation's;
  after a `FULL OUTER JOIN USING` the message names both qualified choices); then a
  relation, standing for its stored columns (generated columns and `rowid` are not key
  columns); else the §9 "neither a column nor a relation" error. Every declared name is
  checked against the FROM columns before any key resolves. Columns register like `T.x`'s, through
  `TableBinding::GetColumnBinding` for a base table, so the scan keeps them.
- The key is sorted and de-duplicated by binding, so equal keys share one
  `EntityScopeInfo`, marked `exact_key`. A `per` scope is never shared with a `T.x` or
  `SUM(T: …)` scope, and only those old scopes drop the columns their clause reads as
  data (decision D9). The variable is `DecideVarScopeInfo::Keyed`: `ENTITY` plus
  `declared_key`, the key as written.
- From there a keyed decision is an `ENTITY` decision: `entity_key_expressions` keep its
  key columns alive through pruning, `plan_decide` resolves them, `BuildEntityMappings`
  groups rows on them with NULL as a value (D5), and every row reads its class's column.
  Layers 5 to 8 are unchanged; layer 4 only words its error for a keyed decision.
- A keyed decision inside `SUM(D: e)` is refused with "not supported yet" (D10; D13
  keeps the refusal until item 7, and `sum(per D: e)` takes one). Until item 5, a keyed
  decision beside a reducer in an aggregate constraint is refused, named as declared,
  with a `per ()` hint (D11). DIAGNOSE labels its scope
  `entity` until item 6 (D11).
- `exact_key` (id 105) and `declared_key` (id 102) are serialized; `nodes.json` names
  `duckdb/planner/operator/decide/logical_decide.hpp`, so regenerating reproduces the
  committed code.
- Tests: `test/decide/tests/test_scope_declare.py` with the reference evaluator
  `_scope_oracle.py` (classes in Python, model through the oracle solver, DeciDB's answer
  judged class by class); the keyed-scope case in
  `test/common/test_decidb_plan_serialization.cpp` (sharing, `exact_key`, round trip);
  the declaration cases of `test/decidb/test_scope_parser.test`, which now run.

## Items 3 and 4 — reducer and clause scope

Built together, under the decisions D12–D18 of [`todo.md`](todo.md) §3. A clause that
uses a new spelling — in front, a `by`, or a scope inside a reducer — follows the ANR
rules; one without keeps today's, so no existing answer moved (the golden models are
identical on both solvers). A written `per row` alone, in front or inside a reducer,
follows the ANR rules too (D19, 2026-10-17). What the user sees: `syntax_reference.md` §10.

- **Binder** (`02_binder/done.md` §4a). `ValidateDecideScopeSpellings` replaces the
  item-1 gate: it keeps "not supported yet" inside ordinary subqueries, refuses mixed
  spellings (D14), scopes on non-reducer calls (D15), window functions in scope data and
  `norm(D: e, p)`. Its whole-number check for `<`, `>` and `<>` counts a constant added
  inside a scoped `MIN` or `MAX`, which stays in the extremum the bound is stepped on.
  `per K:` binds to a `PER_CLASS_TAG` wrapper, `when c:` to a WHEN wrapper, `by (Γ)` to a
  `__group_key_<i>__` tag naming `decide_group_keys[i]` (one entry per key as written, a
  subquery in it included), an inner `per` to a qualifier tag on an exact key scope, an
  inner `when` to the aggregate's filter; every comparison of such a clause carries
  `SCOPED_CLAUSE_TAG`, the one switch later layers read. The two-level objective binds
  onto today's nested objective.
- **Logical plan** (`03_logical_plan/done.md`). `LogicalDecide::group_keys`,
  `scope_checks` and `scoped_objective`, serialized (ids 217–219), in
  `EnumerateExpressions`, flattened by `PlanSubqueries`.
- **Canonicalizer** (`04_canonicalizer/done.md` §3.10, §4 "Scope checks"). `ClassifyScoped` decides a
  scoped comparison's shape — rows, aggregate (stamped with its `by` key; a `per K:`
  wrapper over it is dropped, the checks keep what `K` requires) or refused with "not
  supported yet", the composed MIN/MAX with a `by` or a bound that is not a constant
  included. A lone negated reducer is spelled with its sign in its factor (`-max(e)` as
  `-1 * max(e)`), and one written with signs around it as the bare term (`-(-max(e))`
  as `max(e)`). `BuildConstraintScopeChecks` / `BuildObjectiveScopeChecks` build the
  run-time checks from the clauses as bound, before canonicalization.
- **Optimizer** (`05_optimizer/done.md`). Scoped easy MIN/MAX keeps its reducer (its
  `when` picks the members, the bound reduces over the group); the hard half keeps the
  `by` key and takes a negative factor inside the extremum (`-2 * max(e)` is
  `min(-2 * e)`); a composed clause folds its `when` into each term. Rows a rewrite adds for a
  scoped clause (IN, ABS, norm L0) keep its `when` and its tag. The linear form records
  `scoped`, the group key and the class key, and counts an objective's decision outside
  reducers once (a product there is refused).
- **Model** (`06_model_formulation/done.md`). `MinMaxObjectiveSpec::member_rows`: a
  scoped MIN/MAX objective's members are the rows its reducer reads, whatever their
  coefficients. `EvaluatedConstraint::rows_stand_with_their_class` keeps implied bounds
  for a `per K:` clause emitted once per class. A scoped hard MIN/MAX or composed clause
  steps a strict bound by one (`EvaluatedConstraint::scoped`,
  `ComposedMinMaxTermData::scoped`), and its row keeps the written bound for a diagnosis
  (`RawConstraint::strict`, `typed_k`).
- **Execution** (`08_execution/done.md` §4a). `RunScopeChecks` runs first; a scoped
  clause computes nothing on a row its `when` drops (`ExecuteOnReadRows`, `KeptRows`):
  a NULL there is never read and an expression that would fail there is never computed.
  It groups by its `by` key with NULL a value, emits a `per K:` rows clause at each
  class's first kept row, evaluates each bound-side reducer over its own groups and
  de-duplicates an inner `per` among the rows its `when` keeps. An empty or constant key
  is one group everywhere (`BuildGroupIds`).
- **Messages** quote the query: a `per`/`by` key and a reducer as written, columns
  qualified by relation over a join; a subquery in an objective's `when`, or a NULL one,
  by its text as DuckDB prints it. DIAGNOSE quotes a stepped strict bound as written. `EXPLAIN` echoes the scope spellings. A reducer or `norm` with no argument
  now gets a binder message in every spelling (it was an internal error, or an empty one).
- **Old spellings** keep their behaviour, wrong answers included (D17); the ones found
  are in `06_issues/bugs/todo.md` ("Old spellings: wrong answers found while building
  items 3–4"). The 21 "not supported yet" expectations of `test_scope_parser.test` now
  show each query's result (D16).
- Tests: `test/decide/tests/test_scope_generation.py` (C3, C4),
  `test_scope_reducer_by.py` (C5, MIN/MAX, AVG, `<>`, refusals),
  `test_scope_reducer_inner.py` (C6), `test_scope_determination.py` (C8),
  `test_scope_objective.py` (C9), `test_scope_deck_examples.py` (deck examples 2, 3, 4,
  6, 7 in both forms; example 7's D8 error), against `_scope_oracle.ScopedModel`, which
  follows the ANR definition literally; `_scope_cases.py` holds the shared data. The
  scoped case of `test/common/test_decidb_plan_serialization.cpp` round-trips the new
  plan fields.

## Item 5 — the value rewrite

Shipped 2026-10-17 under D23–D25 of [`todo.md`](todo.md) §3, on the design of its §4.2.
A scoped comparison whose decision side mixes a decision reducer with something the
grouped row cannot hold — a per-row or keyed decision, a factor that varies by row, a
second `by` key, a reducer inside another expression — is the **VALUE shape**: each
decision reducer becomes a helper decision with one instance per `by` group, defined
once, and the comparison is then one row per class. Deck examples 1, 5 and 8 run; example
1's bare form keeps today's refusal until item 7 (D23). What the user sees:
`syntax_reference.md` §10.1 and §10.6.

- **Canonicalizer** (`04_canonicalizer/done.md` §3.10). `ClassifyScoped` returns VALUE
  for the shapes above (`CanonicalConstraintClass::VALUE`), AGGREGATE where the grouped
  row is exact (one `by` key, whole-query factors, only query-wide terms beside; the
  composed MIN/MAX formulation only with no `by`, a constant bound and no decision
  beside), UNSUPPORTED for `AVG` under `<>` with anything but plain AVGs and for a
  query-wide decision beside a reducer whose body multiplies decisions. `PeelScale` lets
  a row-varying factor stay on a scoped constraint's reducer as its scale.
  `CanonicalizeTree` stamps `VALUE_REWRITE_TAG`; the per-class wrapper stays.
- **Logical plan** (`03_logical_plan/done.md`). `LogicalDecide::reducer_value_links`,
  one per occurrence of a reducer in a clause (`ReducerValueLink`: the helper, the
  reducer's kind, body, label, own `when` and `per`, its `by` key, the clause's `when`,
  the MIN/MAX sides any row of the clause needs, the clause id and removal group;
  `inner_terms` flattened by `BuildDecidePreparedModel`), in `EnumerateExpressions`,
  never serialized. `EntityScopeInfo::value_link` marks a helper's scope — the link's
  `by` groups among the rows its clause keeps — so `entity_key_bindings` is empty; the
  field is not serialized.
- **Optimizer** (`05_optimizer/done.md`). `RewriteScopedReducers`
  (`decide_rewrite_scoped.cpp`), after `RewriteBilinear` and before the MIN/MAX passes:
  walks the VALUE-tagged comparisons, reads each reducer's sign through `+`, `-`, casts
  and factors (`ScaleSignAtPlanTime`; 0 for a data factor or an enclosing expression),
  refuses a body that still multiplies decisions (spec §12), and replaces the reducer
  node in place by its helper: the clause's existing one for a repeated occurrence (the
  ABS envelope pair holds two copies; `Helpers::Find` matches reducer, alias, clause id
  and `when`), else a new `__value_<n>__` through `AddAuxiliaryDecision` — INTEGER when
  the reducer's type is integral, `Scalar()` for `by ()`, else `Entity` of its own scope,
  labelled by the reducer's SQL. A MIN/MAX helper needs its envelope when a row pushes it
  down (MAX) or up (MIN), its closing side otherwise, both for `=`, `<>` or an unknown
  sign (`ExtremumPushOf`, shared with the MIN/MAX passes); a shared helper takes every
  side any of its rows needs.
- **Model** (`06_model_formulation/done.md` §9). `DeriveReducerValueBounds` boxes each
  helper by its members' reach over every group (sums for SUM/AVG, cover for MIN/MAX),
  tightening only, with no default floor; `FormulateModel` alternates it with the ABS
  derivation to a fixed point when a model has helpers (a member may be an ABS auxiliary
  and an ABS may hold a helper), then lets the ABS derivation refuse on the lowering arm.
  `LinearizeReducerValueLinks` states each definition: a SUM/AVG helper's flat equality
  per group (`value - members = constants`, `global_constraints`) and a MIN/MAX helper's
  extremum link per group through `EmitExtremumLink`, its closing Big-M the helper's box
  end against the family's far end; native or lowered per the backend policy.
- **Execution** (`08_execution/done.md` §4, §4a). `BuildEntityMappings` builds a helper's
  scope from its link: the `by` key over the rows the clause keeps (NULL a value), the
  dropped rows on one spare entity. `EvaluateReducerValueLinks` computes each reducer's
  members on the rows the clause and the reducer keep (`EvaluateTermColumns`), grouped
  by its `by` key over the kept rows, de-duplicates its own `per` — a dropped row leaves
  its group — and scales AVG; the same drop applies to a hard MIN/MAX of the aggregate
  shape. `BuildSolverInput` frees the helper's floor before the rigid box is captured. A
  refusal names a helper by its reducer.
- **Messages.** DIAGNOSE names the user's clause and never a helper or a definition row
  (structural). EXPLAIN shows a helper as `__value_<n>__` in its solver-row layer, as it
  shows an ABS auxiliary (item 6 labels it).
- Tests: `test/decide/tests/test_scope_value_rewrite.py` (C7: the shapes, MIN/MAX
  sides, strict and `<>`, AVG, inner `when`/`per`, NULL groups, ABS/POWER over a helper,
  linear model size, the unbounded-member refusal on HiGHS against the native answer,
  the §12 refusal, the checks, DIAGNOSE, EXPLAIN, HiGHS agreement; the shared helper
  under a hard-direction ABS on both solvers against a brute-force optimum, a helper
  inside a hard ABS on the lowering arm, a helper key read on the kept rows only, a MIN
  helper over a keyed decision counting each class once), `test_scope_reducer_by.py` (a
  hard MIN/MAX of the aggregate shape counting each class once),
  `test_scope_combinations.py` (§8 grid: 784 cells against the oracle — every cell the
  oracle solves is solved identically, every cell it refuses is refused with the same
  phrase, and no cell answers "not supported yet", asserted), `test_scope_deck_examples.py`
  (examples 1, 5, 8 in both forms, C11), golden corpus entries 95–101 on both solvers.
  The 16 refusal
  cases of `test_scope_reducer_by.py` that item 5 lifted moved to the new suite as
  answers (D25).
