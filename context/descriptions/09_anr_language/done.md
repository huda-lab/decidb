# ANR language surface — implemented

All seven items shipped. The language is
[`../00_project_overview/syntax_reference.md`](../00_project_overview/syntax_reference.md)
§1.1–§7 and §10: `per` and `when` in front of a declaration, a constraint or the
objective, `by` after a reducer, and `when`/`per` inside one. This file records what each
item built, layer by layer, and the decisions the code and tests cite by number
([Decisions](#decisions)). How the scopes behave is in
[`../03_expressivity/scope/done.md`](../03_expressivity/scope/done.md); the scope test
suites and their oracle are in [`../04_testing/scope/done.md`](../04_testing/scope/done.md).

Sources: the "DeciQL proposals" deck (`ANR.pdf`, pp. 6–7, 14–32, 62) and
`ANR Implementation.pdf` (semantics). They are context only: a construct is in the
language because `syntax_reference.md` documents it, not because a PDF shows it.

Open work lives with its owner: `../01_pipeline/02_binder/todo.md` (nested aggregates,
bind-time refusals a later change may lift), `../01_pipeline/03_logical_plan/todo.md` and
`../01_pipeline/08_execution/todo.md` (one key table, one key-grouping path),
`../01_pipeline/04_canonicalizer/todo.md` and `../01_pipeline/05_optimizer/todo.md`
(shapes and formulations), `../03_expressivity/scope/todo.md` (the keyed-scope rename),
`../03_expressivity/explain/todo.md` (columns over a join),
`../07_query_diagnostics/infeasible/todo.md` (the `=` half, the NULL class label),
`../05_performance/04_model_construction/todo.md` (a helper's box per instance) and
`../06_issues/code_quality/todo.md` (the structural follow-ups).

## Item 1 — the scope grammar

- The grammar accepts `per K: x(TYPE)` in a declaration, `[when c] [per K]:` in front of a
  constraint, an objective or a reducer's first argument, and `by (G)` after a function
  call (the binder admits it on `SUM`, `AVG`, `MIN`, `MAX`, `norm` and `count`, D34/D35),
  the colon form `SUM(D: e)` included. Statements print back in these spellings and
  re-parse to an equal statement.
- Inside a DECIDE clause, subqueries written there included, `when`, `per`, a `by`
  directly after `)` and a `row` directly after `per` are keywords; a column with such a
  name is written quoted there (`"row"`). The lexer emits `PER_DECIDE`,
  `PER_DECIDE_PAREN`, `PER_DECIDE_ROW` and `BY_DECIDE` only inside the clause, so
  ordinary SQL — including `GROUP BY` in a subquery written inside the clause — is
  untouched (S7). Details:
  [`../01_pipeline/01_parser/done.md`](../01_pipeline/01_parser/done.md) §2.
- Three parse errors are exact: `per` other than `()` on an objective, a parenthesized
  key (S6), and an expression in a `per` key (S8); messages in `syntax_reference.md`
  §10.1.
- `per row` is the default written out (S1). It builds `PG_AEXPR_SCOPE_PER_ROW`, bound
  as `SCOPE_PER_ROW_TAG` and peeled, so a clause written with it is the bare clause (D19).
- A scope inside an ordinary SQL subquery written in the clause answers
  *"DECIDE: 'WHEN condition:' is not supported yet"* (or `'PER key:'`, `'PER ():'`,
  `'PER ROW:'`, `'BY (key)'`), from `ValidateDecideScopeSpellings`.
- `grammar.y` declares `%expect 4`, all from the optional declaration slot; `per` and a
  reducer's `by` reach the grammar as DECIDE-only tokens, so neither is read as an
  identifier inside the clause (`01_parser/done.md`, "Conflicts").
- Tests: `test/decidb/test_scope_parser.test` (accept, exact errors, boundaries, the
  removed spellings and plain SQL through the engine) and the scope case in
  `test/common/test_decidb_plan_serialization.cpp` (round trips including deck examples
  1–8 as written and fully explicit, a scope covering exactly one constraint, `per row`
  equivalence, parse errors at the parser).

## Item 2 — `decide per K: x(TYPE)`

- A declaration's key binds in `DecideDeclarationsBinder::BindDeclarations`. `per ()` is
  the query-wide scope; no `per` (or `per row`) is per row; `per K:` gets a key scope.
- `FindOrCreateKeyScope` (`decide_binder.cpp`) resolves each key element against the
  query's own `FROM` clause, never an outer query, spelled as SQL spells it (`c`, `R.c`,
  `S.R.c`; `R`, `S.R`): a declared decision is *"x is a decision; when, per and by may only
  use data"*; then a column (an unqualified `USING` column means its primary relation's;
  after a `FULL OUTER JOIN USING` the message names both qualified choices); then a
  relation, standing for every stored column (generated columns and `rowid` are not key
  columns); else *"... is neither a column nor a relation of the FROM clause"*
  (`syntax_reference.md` §10.1). Every declared name is checked against the FROM columns
  before any key resolves. Columns register through `TableBinding::GetColumnBinding` for
  a base table, so the scan keeps them.
- The key is sorted and de-duplicated by binding (`SortedKey`), so equal keys share one
  `EntityScopeInfo` (`FindOrCreateExactScope`, shared with the colon form). The variable
  is `DecideVarScopeInfo::Keyed`: the `ENTITY` scope plus `declared_key`, the key as
  written.
- From there a keyed decision is an `ENTITY` decision: `entity_key_expressions` keep its
  key columns alive through pruning, `plan_decide` resolves them, `BuildEntityMappings`
  groups rows on them with NULL as a value (D5), and every row reads its class's column.
  DIAGNOSE's `scope` column labels it `entity`; the rename to `keyed` is
  `../03_expressivity/scope/todo.md`.
- A keyed decision inside `SUM(D: e)` answers where `D` determines it: the colon form is
  `sum(per D: e)`, with the same scope and run-time data check (S4).
- `declared_key` (id 102) is serialized; `nodes.json` names
  `duckdb/planner/operator/decide/logical_decide.hpp`, so regenerating reproduces the
  committed code.
- Tests: `test/decide/tests/test_scope_declare.py` with the reference evaluator
  `_scope_oracle.py` (classes in Python, model through the oracle solver, DeciDB's answer
  judged class by class); the keyed-scope case in
  `test/common/test_decidb_plan_serialization.cpp` (sharing, round trip); the declaration
  cases of `test/decidb/test_scope_parser.test`.

## Items 3 and 4 — reducer and clause scope

Built together, under D3–D8, D15 and D18. What the user sees: `syntax_reference.md`
§5.2–§5.4, §6 and §7.

- **Binder** (`02_binder/done.md` §4a). `ValidateDecideScopeSpellings` keeps "not
  supported yet" inside ordinary subqueries and refuses a scope or `by` on a call that is
  not a reducer (D15), a decision or window function in scope data, and `norm(D: e, p)`.
  Its whole-number check for `<`, `>` and `<>` counts a constant added inside a `MIN` or
  `MAX`, which stays in the extremum the bound is stepped on. `per K:` binds to a
  `PER_CLASS_TAG` wrapper, `when c:` to a WHEN wrapper (`WHEN_CONSTRAINT_TAG`), `by (G)`
  to a `__group_key_<i>__` tag naming `decide_group_keys[i]` (one entry per key as
  written, a subquery in it included), an inner `per` to a `__qualified_by_<i>__` tag on
  an exact key scope, an inner `when` to the aggregate's filter. The two-level objective
  binds onto the nested objective (`PER_CONSTRAINT_TAG`, `DecideObjective::per_columns`).
- **Logical plan** (`03_logical_plan/done.md`). `LogicalDecide::group_keys` and
  `scope_checks`, serialized (ids 217–218), in `EnumerateExpressions`, flattened by
  `PlanSubqueries`.
- **Canonicalizer** (`04_canonicalizer/done.md` §3.10, §4 "Scope checks").
  `ClassifyScoped` decides a comparison's shape — rows, aggregate (stamped with its `by`
  key, the `per K:` wrapper kept over it) or refused with its message. A lone negated
  reducer is spelled with its sign in its factor (`-max(e)` as `-1 * max(e)`), and one
  written with signs around it as the bare term (`-(-max(e))` as `max(e)`).
  `BuildConstraintScopeChecks` / `BuildObjectiveScopeChecks` build the run-time checks
  from the clauses as bound, before canonicalization.
- **Optimizer** (`05_optimizer/done.md`). An easy MIN/MAX keeps its reducer (its `when`
  picks the members, the bound reduces over the group) unless bound absorption folds it
  into the decision's box (D41); the hard half keeps the `by` key
  and takes a negative factor inside the extremum (`-2 * max(e)` is `min(-2 * e)`); a
  composed clause folds its `when` into each term. Rows a rewrite adds for a clause (IN,
  ABS, norm L0) keep its `when`. The linear form records the group key and the class
  key, and counts an objective's decision outside reducers once; a square or a product
  of decisions there is refused unless the two are distinct query-wide decisions or one
  is `BOOL` (`syntax_reference.md` §4).
- **Model** (`06_model_formulation/done.md`). `MinMaxObjectiveSpec::member_rows`: a
  MIN/MAX objective's members are the rows its reducer reads, whatever their
  coefficients. `EvaluatedConstraint::rows_stand_with_their_class` keeps implied bounds
  for a `per K:` clause emitted once per class. A hard MIN/MAX or composed clause steps a
  strict bound onto whole numbers, and its row keeps the written bound for a diagnosis
  (`RawConstraint::strict`, `typed_k`).
- **Execution** (`08_execution/done.md` §4a, §5). `RunScopeChecks` runs first; a clause
  computes nothing on a row its `when` drops (`ExecuteOnReadRows`, `KeptRows`): a NULL
  there is never read and an expression that would fail there is never computed. It
  groups by its `by` key with NULL a value, emits a `per K:` rows clause at each class's
  first kept row, evaluates each bound-side reducer over its own groups and
  de-duplicates an inner `per` among the rows its `when` keeps. An empty or constant key
  is one group everywhere (`BuildGroupIds`).
- **Messages** quote the query: a `per`/`by` key and a reducer as written, columns
  qualified by relation over a join; a subquery in an objective's `when`, or a NULL one,
  by its text as DuckDB prints it. DIAGNOSE quotes a stepped strict bound as written. A
  reducer or `norm` with no argument gets a binder message (D22).
- Tests: `test/decide/tests/test_scope_generation.py` (constraint `when` and `per`),
  `test_scope_reducer_by.py` (`by`, MIN/MAX, AVG, `<>`, refusals),
  `test_scope_reducer_inner.py` (a reducer's own `when` and `per`),
  `test_scope_determination.py` (the determination rule), `test_scope_objective.py`
  (the objective), `test_scope_deck_examples.py` (deck examples 2, 3, 4, 6, 7 as written
  and fully explicit; example 7's D8 error), against `_scope_oracle.ScopedModel`, which
  follows the ANR definition literally; `_scope_cases.py` holds the shared data. The
  scope case of `test/common/test_decidb_plan_serialization.cpp` round-trips the plan
  fields.

## Item 5 — the value rewrite

Shipped 2026-10-17 under D24. A comparison whose decision side mixes a decision reducer
with something the grouped row cannot hold — a per-row or keyed decision, a factor that
varies by row, a second `by` key, a reducer inside another expression — is the **VALUE
shape**: each decision reducer becomes a helper decision with one instance per `by`
group, defined once, and the comparison is then one row per class. Deck examples 1, 5
and 8 run, example 1 as written (`ship <= 0.20 * sum(ship)`). What the user sees:
`syntax_reference.md` §6.4, §10.1 and §10.2.

- **Canonicalizer** (`04_canonicalizer/done.md` §3.10). `ClassifyScoped` returns VALUE
  for the shapes above (`CanonicalConstraintClass::VALUE`), AGGREGATE where the grouped
  row is exact (one `by` key, whole-query factors, only query-wide terms beside; the
  composed MIN/MAX formulation only with no `by`, a constant bound and no decision
  beside), UNSUPPORTED for `AVG` under `<>` with anything but plain AVGs. `PeelScale` lets
  a row-varying factor stay on a constraint's reducer as its scale.
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
  refuses a body that still multiplies decisions (`syntax_reference.md` §10.2), and
  replaces the reducer node in place by its helper: the clause's existing one for a
  repeated occurrence (the ABS envelope pair holds two copies; `Helpers::Find` matches
  reducer, alias, clause id and `when`), else a new `__value_<n>__` through
  `AddAuxiliaryDecision` — INTEGER when the reducer's value is whole
  (`DecideValueIsWholeNumbered`; a strict or `<>` row over a fractional one is refused),
  `Scalar()` for `by ()`, else `Entity` of its own scope, labelled by the reducer's SQL.
  A MIN/MAX helper needs its envelope when a row pushes it down (MAX) or up (MIN), its
  closing side otherwise, both for `=`, `<>` or an unknown sign (`ExtremumPushOf`, shared
  with the MIN/MAX passes); a shared helper takes every side any of its rows needs.
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
  (structural). EXPLAIN names a helper by its reducer and says it became a helper
  decision.
- Tests: `test/decide/tests/test_scope_value_rewrite.py` (the shapes, MIN/MAX sides,
  strict and `<>`, AVG, inner `when`/`per`, NULL groups, ABS/POWER over a helper, linear
  model size, the unbounded-member refusal on HiGHS against the native answer, the
  product-inside-a-helper refusal, the checks, DIAGNOSE, EXPLAIN, HiGHS agreement; the
  shared helper under a hard-direction ABS on both solvers against a brute-force optimum,
  a helper inside a hard ABS on the lowering arm, a helper key read on the kept rows
  only, a MIN helper over a keyed decision counting each class once),
  `test_scope_reducer_by.py` (a hard MIN/MAX of the aggregate shape counting each class
  once), `test_scope_combinations.py` (784 cells against the oracle: every cell the
  oracle solves is solved identically, every cell it refuses is refused with the same
  phrase, and no cell answers "not supported yet", asserted),
  `test_scope_deck_examples.py` (examples 1, 5, 8 as written and fully explicit), golden
  corpus entries 95–101 on both solvers.

## Item 6 — EXPLAIN, DIAGNOSE and the tests in the language

Shipped 2026-10-18 under D27 and D28. EXPLAIN and DIAGNOSE print every clause in the
language, and the tests, the golden corpus and the benchmarks are written in it. What
the user sees: `syntax_reference.md` §8.1 and §9.

- **EXPLAIN** (`03_logical_plan/done.md`, `05_optimizer/done.md`). Every renderer takes a
  `DecideSourceNames` that carries the auxiliary labels, so a decision column the
  optimizer added prints as what it stands for — `abs(e)`, `(e >= 0)`, `(b * x)`,
  `(x = v)`, `(e <> 0)`, `sum(e) by (g)` — in both plans, a later rewrite's label
  included (D27). Each rewrite records a fact, `LogicalDecide::RewriteAccount` (subject
  and `Kind`, under its clause or the objective), which `RenderDecideRewriteAccount` words
  once: "becomes an auxiliary variable [with a sign indicator | stated to the solver as an
  absolute value | linearized by McCormick rows]", "one indicator per value / per row",
  "a helper decision [per group]", "an extremum column [per group] [pinned by indicator
  rows]", "two conditional rows with an indicator", "a sum divided by its row count"
  (AVG), "a sum of absolute values / of squares", "a maximum of absolute values" (`norm`),
  "a bound on each member" (a MIN/MAX in its cheap direction); EXPLAIN prints them under
  the clause before its rows, and under the objective with the rows its rewrites added
  for it (`OBJECTIVE_ROW_TAG`). A row that repeats its clause is left out; the hard
  MIN/MAX carrier prints as the reducer it marks; a MIN/MAX or composed objective prints
  its accounts and no carrier row, an AVG objective its sum under its account; an
  automatic L0 bound prints as `M`; the two-level objective's rewritten row reads
  `when c per k: inner(e) by (k)`, its key as written (`RenderDecidePerKey`); functions
  print lower-case and a column is quoted where SQL needs it (`"order"`, `"per"`); the
  rows a rewrite adds keep their clause's `when` in front; `norm(e, 'inf')` keeps its
  quotes. Parentheses follow DuckDB's precedence in every echo: `-(-x)`,
  `x * (cap % 7)`, `(a OR b) AND c`, `a ** (b ** c)`.
- **DIAGNOSE** (`07_query_diagnostics/infeasible/done.md`, `08_execution/done.md` §4a,
  `06_model_formulation/done.md`). The `group` column holds the `per` key value (D28, ANR
  deck p. 16–17 and 110): the canonicalizer keeps the `per K:` wrapper over the aggregate
  shape, the linear form reads it as `DecideConstraint::PerKeyRole` (`NAMES_CLASSES`) with
  the class key, and under DIAGNOSE execution records the classes each `by` group's row
  stands for (`CollectGroupClasses` → `SolverInput::class_sets`, referenced by
  `EvaluatedConstraint::group_class_set` and `RawConstraint::class_set`, carried to
  `ConstraintProvenance::class_set`). In the expanded slack scope `FanOutByClass` reports
  the group once per class — `expanded_group` with the key value, or `expanded_row` per
  kept row when the clause has no `per` key (D33) — for SUM/AVG rows, hard and easy
  MIN/MAX, composed clauses and helper-path clauses alike (an easy MIN/MAX's members fold
  into one slack per group); a per-row row of a clause with no `per` key keeps its own
  slack and reports at its data row (`ConstraintProvenance::data_row`, `per_kept_row`).
  `per ()` (a `WHOLE_CLAUSE` class set) and the query scope report once. A bound the
  optimizer folds into a column box (`x >= 18`, an ABS pin, a keyed decision's bound)
  reaches the repair model through its own clause
  (`DecidePreparedModel::absorbed_constraints`, evaluated under DIAGNOSE only), so it
  reports per class too; such a bound's strict `<`/`>` steps onto whole numbers
  (`x > 2.5` is `x >= 3`). A kept row's finding over a canonicalized bound offsets the
  bound as written (`x >= 0.5 * sum(x) by (g) + 20 - 20`), a strict bound keeping its
  operator and an `=` moving by its signed slack. A quadratic row keeps its own slack and
  reports its classes in the expanded scope. A composed clause takes its repair id after
  every other clause's, so a row formulation drops (a `<>`) never lands it on another
  clause's slack. An unreachable bound names the `per` key's values in either scope, the
  kept rows in the expanded scope only. The unbounded remedy is written
  `when region = 'B': buy <= <cap>`, the column as the clause names it (`t.depot` for a
  key column) and the value a SQL string. A HiGHS refusal quotes a clause with its scope
  in front. An outer `avg` in the two-level objective is refused.
- **Conversion.** The pytest suites, the golden corpus (identical dumps and results on
  both solvers) and benchmarks p4, q1, q4, q6 and q8 are written in the language.
- Tests: `test/decide/tests/test_scope_explain_diagnose.py` (accounts and labels per
  rewrite kind — AVG, `norm` 1/2/inf/0, easy and hard MIN/MAX, `=`, product, `IN`,
  `<>`, helpers — stacked rewrites, the objective's accounts and rows, the L0 `M`,
  lower-case functions, the two-level objective's layers, a scoped `IN`, the outer-avg
  refusal; DIAGNOSE per class for a finer `per` key, an equal one, no key, a hard
  MIN/MAX, an easy MIN/MAX with a literal and a group bound, a composed clause, a `when`
  in front, `per ()` and its `=`, the query scope, the infinity norm's echo pasted back,
  lower-case functions, a bound folded into a column box — ABS pin, `per row:`, keyed
  decision —, a kept row's offset written bound and its strict and `=` forms,
  unreachable bounds in both scopes, a composed clause's own slack, quadratic classes,
  the remedy's key column, a two-level key in EXPLAIN, the HiGHS refusal's scope),
  `test_scope_generation.py` (a fractional strict bound),
  `test_query_diagnostics_relation.py` (`TestComposedClauseRepairIdentity`), the
  converted EXPLAIN and diagnostics pins, `test_decidb_diagnostic_engines.cpp`'s remedy
  pin.

## Item 7 — one language

Shipped 2026-10-19 under D30–D56 and D60–D63. Every clause follows the scope rules, bare
clauses included (D30): an omitted `per` is `per row`, an omitted `by` is `by ()`, an
omitted `when` keeps every row. The spellings the language replaced are refused, each
with a message that carries the phrase "is removed" and names the spelling to write,
built from the user's own text. The behaviour specification is folded into
`syntax_reference.md`, and the plan's open items moved to their owners (intro). What
the user sees: `syntax_reference.md` §10.3 (every removed spelling and its message),
§1.1 (defaults), §6.4 and §7.

- **Parser** (`01_parser/done.md` §1 "Productions kept for the removed spellings", §3
  "Refusing the removed spellings"). The productions of the removed forms stay in
  `select.y` and build their markers (`PG_AEXPR_WHEN_CONSTRAINT`,
  `PG_AEXPR_PER_CONSTRAINT`); `RefuseRemovedDecideSpellings`
  (`src/parser/decide/decide_removed_spellings.cpp`), called from the select-node
  transformer, throws a `ParserException` at the first one (D37, D62). A removed
  declaration (`T.x(TYPE)`, `scalar x(TYPE)`, either without a type) is refused in the
  grammar with the same messages (`decideTypeWord`; D50, D56); `per (): cap` with no type
  gets "needs a type". The comma form `C PER g, h` folds its column into the marker
  (`absorbRemovedPerKey`), so the message names the whole key. A comma between
  constraints is refused (*"use AND between constraints"*). A postfix form after a clause
  that already has a scope in front is a plain syntax error. `SCALAR` stays an unreserved
  keyword used only by the error rule, so `scalar` is an ordinary identifier (D51). The
  constraint postfix `WHEN` takes an `a_expr`, so `WHEN NOT f` is named as written (D55).
  `decide_parse_hints.cpp` is deleted; the predicates over the parsed markers that the
  refusal and the binder share live in `src/parser/decide/decide_parsed_markers.cpp`.
  A removed `WHEN`'s condition is quoted whole as written, rebuilt from the operators the
  old grammar parsed around the marker (`WrittenCondition`); a reducer is read and spelled
  once (`ReadReducer`, `ReducerSpelling`), so a `WHEN` after a reducer with a scope or a
  `by` joins its `when`; the NULL note keeps the clause's own `when`, and the nested and
  flat-`AVG` objective forms carry it too, since those dropped NULL keys; a declaration's
  names are quoted as DuckDB prints them (`decideName`); every select node is checked,
  so a removed form inside a subquery of a clause is named.
- **Binder** (`02_binder/done.md` §1, §3 "Relation-qualified reducers", §4, §4a).
  `RefuseQualifiedDecisionReferences` refuses `D.x` for a decision over the constraints,
  the objective, the select list and `ORDER BY`, before the scopes are read (D43, S2); the
  same `RefuseRemovedDecideSpellings` runs at the top of `BindDeclarations` for a parse
  tree stored before the removal (D54). The colon form follows S4 (D38):
  `BindQualifiedReducer` builds an exact key over the listed relations' stored columns
  (`AddRelationColumns`, `SortedKey`, `FindOrCreateExactScope`) and tags the reducer
  `COLON_FORM_TAG`, so it echoes as written; its data check runs at run time
  (*"per D does not determine ..."*, D63) and a keyed decision inside it answers. `by` is
  admitted on `norm(e, p)` and `count(*)` (D34, D35). The whole-number gate
  (`ValidateDecideIntegralComparisonOperands`) applies to every clause, and
  `DecideValueIsWholeNumbered` gives the binder and the value rewrite one test.
  `DecideCaseUnsupportedMessage` names the `when` and `per` scopes. Deleted: the `T.x`
  declaration path and its key trimming, `RewriteScopedVarRefs`,
  `CheckQualifiedReducerBody`, `FindOrCreateEntityScope` and `table_scope_map`,
  `BindWhenConstraint`, `BindPerConstraint`, `BindLocalWhenAggregate`, `BindPerWrapper`,
  `UsesOldSpelling`, `binding_old_clause`, the no-mixing check, the objective's postfix
  branches, and `SCOPED_CLAUSE_TAG` with every writer and reader.
- **Logical plan** (`03_logical_plan/done.md`). `LogicalDecide::scoped_objective` (id 219)
  and `EntityScopeInfo::exact_key` (id 105) are deleted and retired as `"deleted": true`
  in the serialization JSON, `serialize_*.cpp` regenerated. `PER_CONSTRAINT_TAG` now wraps
  only the two-level objective; a constraint's `per K:` is `PER_CLASS_TAG`.
- **Canonicalizer** (`04_canonicalizer/done.md` §3.7, §3.10, §4). `ClassifyScoped`
  classifies every comparison, and an `INVALID` one throws its refusal (or an
  `InternalException` when a decision survived on the bound side); `FindNonScalarDecideVar`,
  `ClassifyCanonicalTree` and their messages are deleted, with the PER rule of
  `ValidateCanonicalTree` and `VerifyCanonicalTree`. A nonlinear reducer body beside a
  query-wide decision stays AGGREGATE (`s + sum(b * x) <= 20`,
  `sum(power(x, 2)) + s <= 30`), and a product of decisions beside the reducers
  (`sum(x) + b * s`) is VALUE. `PeelScale` refuses a decision as a factor on a reducer
  (`b * sum(x)`) and a row-varying factor on one in the objective.
- **Optimizer** (`05_optimizer/done.md`). A `<>` over a square or a product of decisions
  left after the bilinear rewrite is refused (`FindNotEqualConstraints`, *"<> over a
  product or a power of decisions is not supported yet"*); its two rows held the linear
  terms only, so it was solved without the rest, and stage 06 now treats one reaching it
  as an internal error. Bound absorption absorbs an easy
  `min(x) >= K` / `max(x) <= K` with no `when` and no reducer filter into the decision's
  box, so a negative `K` makes `x` signed (D41); it recurses through `per K:` and stops at
  `when`, and every absorbed bound is recorded in
  `DecidePreparedModel::absorbed_constraints`. The linear form and the composed clause
  (`ScopeComposedTerms`) read a clause's scope with no `scoped` flag.
- **Model** (`06_model_formulation/done.md`). Every `scoped` field is gone; strict
  stepping onto whole numbers holds for every hard MIN/MAX and composed clause. The
  qualifier text comes from the source registry (`ConstraintSourceInfo::qualifier`,
  read through `SourceAwareProvenance`). The diagnosis labels of a MIN/MAX extremum column
  and a `<>` indicator are spelled as the plan prints the clause (`RenderForPlan`). A
  product of two query-wide decisions outside a reducer counts once
  (`DecideObjective::BilinearTerm::reduction`, `CountOnce` at execution), and the
  quadratic builder counts a query-wide term beside the reducers once per row set.
- **Execution** (`08_execution/done.md` §1, §2, §4, §4a, §5). One path reads a clause:
  the constraint-level PER wrapper, `DecideConstraint::per_columns`, the dense
  NULL-excluded group numbering, `RejectEmptyAggregate` and `null_excludes` are deleted;
  NULL is a key value everywhere (D5) and the D4/D8 checks of `RunScopeChecks` are the
  only empty-set rules. `EmitAbsorbedUserBoundRows` is deleted: an absorbed bound reaches
  DIAGNOSE through `absorbed_constraints` like any clause.
- **Echo** (`syntax_reference.md` §9). One style for every clause: reducers and keywords
  lower-case, the `when c per K:` qualifier in front, a reducer's `by (G)` after it, the
  defaults `per row:` and `by ()` not printed (D32); the colon form as written (D38).
  The postfix renderer arms are deleted, and `AttachDecideQualifier` writes a qualifier
  in front only. Expanded DIAGNOSE reports per kept row for a clause with no `per` key
  (D33).
- **Fixed with it** (D40, the logged bugs the single path reaches, and the final
  review): `<>` over a square or a product of decisions is refused, not solved without
  those terms; `abs(x) <= -1` names `abs(x)`, not an internal column; `OR` between
  constraints is refused, not read as `AND`; a data-only reducer on the decision side is
  refused with a message (S10); no internal name reaches "does not reference a decision";
  `sum(x) + s <> 5` counts `s` once per group; a constant counts under a power, a product
  of decisions and a norm of order 1, 2 or `'inf'` (`POWER(x + 0.25, 2) < 11` is refused,
  not stepped); `x + NULL` beside a decision is refused at bind; a helper decision is
  never read back into a result column, so a range error cannot name `__value_<n>__`; a
  signed MIN/MAX bounded by a column answers on HiGHS; a generated column is not a key
  column; `MAX(x) <= MAX(v)` answers, and DIAGNOSE of a hard MIN/MAX over a keyed decision
  suggests a change (these two have no regression test yet:
  `../06_issues/code_quality/todo.md`).

**Answers that changed**, each pinned by a regression test:

- A NULL key value is its own class under `per` and its own group under `by` (D5);
  `when g is not null per g:` leaves those rows out, as the removed-`PER` message says.
- A `when` that keeps no row is an error (*"DECIDE: when g = 'z' matches no rows."*, D4),
  and so is a group a reducer's own `when` empties (*"... has no rows for g = 'a'."*,
  D8, D18); both were skipped silently.
- Under an explicit `per`, a bound that varies within a class is the determination error
  (*"per g does not determine cap. g = 'a' has more than one value: 3, 5."*); with no
  `per`, `sum(x) <= cap` still takes the tightest bound (S3).
- A flat `SUM(e) PER g` objective is refused, naming `sum(e)` (*"an objective is produced
  once"*).
- A strict bound steps onto whole numbers: `MAX(x) > 12` gives 13, `MIN(x) < 3` gives 2,
  `SUM(x) + MAX(x) > 30` is `>= 31`, an absorbed `x > 2.5` is `x >= 3`. A negative factor
  on a hard MIN/MAX no longer reverses the clause (`-2 * MAX(x) <= -10`).
- A bare clause follows the scope rules (D30): deck example 1's `ship <= 0.20 * sum(ship)`
  runs as written; a row-varying factor on a reducer in a constraint (`cap * sum(x) <=
  60`) and a decision beside a reducer are answered (VALUE shape); an easy
  `min(x) >= -5` makes `x` signed under every scope (D41); a MIN/MAX objective counts a
  member whose coefficient is zero; `MAXIMIZE sum(x) - w * s` with a row-varying `w` is
  the determination error (*"per () does not determine w"*); a query-wide decision
  squared in the objective, and a reducer inside a reducer, are refused.
- Golden corpus entry 44 (`3 - MAX(x) <= 0`, a hard MAX with a negative factor) is
  re-captured on both solvers: 10 rows become 6, the optimum is the same and the tie
  between rows resolves differently (`x = 3` on row 4 instead of row 1).

**Tests**, by category (3,153 DECIDE tests pass; the golden corpus is identical on Gurobi
and HiGHS):

- New suites: `test_scope_removed_syntax.py` (every removed spelling in both clause
  orders and in nested, stored and EXPLAIN/DIAGNOSE queries, each message pinned and
  pasted back; `D.x` in every position; the colon form still accepted; a postfix after a
  front scope a syntax error), `test_scope_bare_clauses.py` (bare clauses under the
  scope rules, D41, the lifted shapes, the fixed bugs), `_brute_force.py` (an enumeration
  reference for shapes `ScopedModel` cannot state), and the scanner
  `test_no_old_spellings_remain.py` with `_old_spellings.py`
  (`../04_testing/scope/done.md`, "Keeping removed spellings out").
- Harness: `_scope_oracle.py` steps strict senses onto whole numbers (D60); the CLI
  runner (`test/decide/decidb_cli.py`) fails a test on an INTERNAL error or an internal
  `__name__` in its output, and on a stderr that says "is removed" unless the test opts in.
- The 90 tests that kept a removed spelling (28 files): converted, rewritten to the
  scope rules and judged by an oracle, moved into the removed-syntax suite as cases, or
  deleted with the internals they pinned (D39, D53).
- Echo pins: the pinned EXPLAIN and DIAGNOSE texts are lower-case and lose the
  synthesized `per row:` (D32); no query or number moved.
- Refusals that became answers (D30): the bare shapes of `test_canonicalize_scale.py`,
  `test_canonicalize_homogeneity.py` and `test_canonicalize_side_agnostic.py`, and the
  keyed decision beside a reducer, each oracle- or brute-force-judged.
- Deliberate comparisons of the two languages in `test_scope_declare.py`,
  `test_scope_reducer_inner.py`, `test_scope_generation.py` and
  `test_qualified_reducer.py`: rewritten to the current rule (S4's run-time data check,
  the quoted echo of a key ending in a colon) or deleted.
- C++: the serialization matrix and the bound round trip in the language, the REQUIREs
  on the retired fields dropped; the diagnostic-engine fixtures lose the `scoped` field
  and write their qualifier in front. `test/decidb/test_scope_parser.test`: the removed
  forms are `statement error` cases.
- Golden corpus entries 62, 66 and 94 in the language (identical dumps; 62 uses
  `count(*) by (grp)`), entry 44 re-captured; the benchmark TAG headers in the language.

**Deviations from the plan**, kept on purpose:

- `WHEN_DECIDE_OBJECTIVE` and `in_decide_objective` stay: the error-only production after
  the objective needs its own token to read a comparison as the condition it names.
- The parsed-tree rendering of the removed markers and of the `scalar_` type marker
  (`function_expression.cpp`, `select_node.cpp`) stays, so a view or macro stored before
  the removal prints as written and is refused when bound (D54).
- `%expect 4` is unchanged: the objective's postfix condition stays a restricted
  comparison (`decide_objective_when_condition`), since an `a_expr` there adds three
  conflicts; a longer condition parses as operators around the marker and the
  transformer reads it back.
- A constant inside a `MIN` or `MAX` under a strict bound (`max(x + 0.5) > 3`) stays
  refused (*"Compare with '>=' instead"*); moving it onto the bound belongs in stage 04
  (`../01_pipeline/02_binder/todo.md` §2).
- The colon form's bind-time row-invariance check was deleted, not moved: the
  decision side already has the parsed-tree check, so `x <= SUM(D: 2)` answers like
  `x <= sum(per D: 2)`.
- The renames (D46) and the structural follow-ups (D47) are a separate change; the
  two key-grouping paths stay unmerged (`../01_pipeline/08_execution/todo.md` §1).

## Decisions

Settled while the language was built; the code and the tests cite them by number. A
number missing from the table was transitional — it governed the period when two
languages coexisted — or was never assigned, and is not reused; D59 concerns the commit,
not the code.

| # | Decision |
|---|---|
| D1 | `SUM(D: e)` stays; it means `sum(per D: e)` (S4) |
| D2 | The nested objective stays, written `outer(per k: inner(e) by (k))` |
| D3 | The clause shapes support `SUM`, `AVG`, `MIN` and `MAX` |
| D4 | A `when` that keeps no rows is an error, with or without a reducer in the clause |
| D5 | NULL is its own class for a declaration's `per`, a constraint's `per` and `by` |
| D6 | `per g: sum(x) <= 10` is accepted; no `by` means `by ()` |
| D7 | Each declarator has its own key; a `per` does not carry over |
| D8 | A reducer with no rows for some class is an error, not a skipped constraint |
| D15 | A scope or `by` on a call other than `SUM`/`AVG`/`MIN`/`MAX` is refused with a message (D34 and D35 admit `by` on `norm` and `count`) |
| D18 | The D8 message names the `by` group the reducer reads (`has no rows for D.region = 'North'.`); with `by ()` it ends `has no rows.` |
| D19 | A written `per row:`, in front of a constraint, inside a reducer or before a declaration, is the default written out (`PG_AEXPR_SCOPE_PER_ROW`, `SCOPE_PER_ROW_TAG`) |
| D22 | A reducer or `norm` with no argument gets a binder message (`SUM needs one argument, found 'sum()'`) |
| D24 | The value rewrite lifts every refusal it handles exactly: a decision beside a reducer, different `by` keys, a row-varying factor, a reducer inside another expression, a `MIN`/`MAX` beside another term with a `by` or a non-constant bound, a decision beside a `MIN`/`MAX` in a constraint, a query-wide product beside a reducer |
| D27 | `EXPLAIN` names every auxiliary by what it stands for and prints one account line per rewrite under its clause |
| D28 | `DIAGNOSE`'s `group` column holds the `per` key value: in the expanded slack scope a `by`-grouped clause reports its group's finding once per class of its `per` key, and once per kept row when it has no `per` key (ANR deck pp. 16–17, 110) |
| D30 | Every clause follows the scope rules, bare clauses included |
| D31 | Each clause's language was decided once, at its root, while two existed; the tag that carried it is deleted |
| D32 | One echo for every clause: lower-case reducers and keywords, the qualifier in front, no synthesized `per row:`, `by ()` not printed |
| D33 | The expanded DIAGNOSE scope reports per kept row for a clause with no `per` key (D28 read literally) |
| D34 | `count(*)` takes a `by`; `count(*) by (g)` counts a group's rows |
| D35 | `norm(e, p)` takes a `by` |
| D36 | A removed objective form with no exact twin gets its own message: a flat `MIN`/`MAX`/`AVG` with `PER` is told that an objective is produced once; an outer `AVG` is pointed to `sum` |
| D37 | Removed-spelling messages carry the fixed phrase "is removed" and quote the user's own expressions, built by the transformer from the parse tree |
| D38 | The colon form takes an exact key on `D`'s stored columns, the run-time data check and a keyed decision inside it, and echoes as written |
| D39 | Test changes are permitted by category, each one listed |
| D40 | `OR` between constraints is refused; a data-only reducer on the decision side is refused with a message; no internal name in "does not reference a decision" |
| D41 | An easy `min(x) >= K` / `max(x) <= K` means `x >= K` / `x <= K` on every row it reads; with no `when` and no reducer filter it is absorbed into the decision's box, so a negative `K` makes `x` signed. Since 2026-10-23 the bound is read by value: any affine or scaled spelling with a constant other side (`min(x) - 1 >= K - 1`, `2 * min(x) >= 2K`, `-min(x) <= -K`) is absorbed like the literal one, for `x` and for `min(x)` alike |
| D42 | Item 7 was built without intermediate commits |
| D43 | `D.x` is refused at bind time only when `D` is a `FROM` relation and `D.x` binds to no column (*"write open, not D.open; a decision is referenced by its bare name"*); in a `when`, `per` or `by`: *"D.open is a decision; when, per and by may only use data"* |
| D44 | The NULL class label in DIAGNOSE's `group` column is open: it prints as the text `NULL` (`../07_query_diagnostics/infeasible/todo.md` §2) |
| D45 | EXPLAIN and DIAGNOSE keep printing a non-key column by its bare name over a join (`../03_expressivity/explain/todo.md`) |
| D46 | The scope enum becomes `ROW / KEYED / GLOBAL` with its values kept, and DIAGNOSE's label `keyed`; a follow-up change (`../03_expressivity/scope/todo.md`) |
| D47 | The structural follow-ups — one clause-scope struct and walker, one two-level renderer — are a follow-up change (`../06_issues/code_quality/todo.md`) |
| D48 | The settled decisions keep their numbers here; the transitional ones are retired |
| D49 | The docs outside `context/descriptions` (`README.md`, `tools/pythonpkg/README.md`, `test/decide/README.md`, the `learn` and `recap` skills, the benchmark TAG headers) are written in the language; `.claude/CLAUDE.md` and the website change only on the user's word |
| D50 | `DECIDE scalar cap` and `DECIDE lineitem.x` (no type) get the removed-spelling error, naming a typed declaration; `per (): cap` gets "needs a type" |
| D51 | `SCALAR` stays an unreserved keyword, used only by the error rule; `scalar` is an ordinary identifier |
| D52 | The paper's Figure 1 query runs in the language (`test_reduced_bound_data_column.py`) |
| D53 | The empty-set message tests keep their identity; their expected text is the D4/D8 phrase |
| D54 | A parse tree stored before the removal (a view, a macro) prints as written and is refused when bound, with the removed-spelling message |
| D55 | The postfix `WHEN` after a constraint takes an `a_expr`, so `WHEN NOT f` and `WHEN g IN (...)` get the named message |
| D56 | The `T.x` message names `per T:` and says that `per T` keys on every column of `T` |
| D60 | The scope oracle steps a strict sense onto whole numbers (`> K` → `floor(K) + 1`, `< K` → `ceil(K) - 1`), identical to stepping by one for a whole `K` |
| D61 | `syntax_reference.md` §10.3 keeps the NULL-key and `per T` bullets beside the table |
| D62 | The removed-`PER` message adds the NULL note: *"a NULL g is now its own group, so write when g is not null per g: to leave those rows out"* |
| D63 | Determination and D8 messages print the colon form as written |

| # | Call |
|---|---|
| S1 | `per row` is accepted as the written-out default |
| S2 | Decisions are referenced by bare name; `D.open` is refused (D43) |
| S3 | With no `per`, a constraint is per row, so `sum(x) <= cap` takes the tightest `cap`; with an explicit `per`, a varying bound is the determination error |
| S4 | `SUM(D: e)` uses the same data check as `sum(per D: e)` |
| S6 | `per (a, b):` is refused with a message; parentheses mean only `per ()` |
| S7 | Inside a DECIDE clause `when`, `per`, and `by` after a `)` are keywords; a column with such a name is quoted there |
| S8 | `per` keys are columns and relations; expressions are allowed only in `by` |
| S9 | An input with no rows at all gives an empty result; D4 is about a `when` that removes every row of a non-empty input |
| S10 | Data-only reducers stay on the bound side of a comparison |
| S11 | A reducer over a lone query-wide decision (`SUM(cap)`) is refused |
