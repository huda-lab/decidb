# ANR language surface — implemented

All seven items shipped, and the full ANR algorithm built on them -- variant signatures,
reducers nested to any depth, and frame selectors (items 8, 9 and 10) -- in the eight
phases recorded below (0 to 7). The language is
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

Open work lives with its owner: `../01_pipeline/02_binder/todo.md` (bind-time refusals
a later change may lift), `../01_pipeline/03_logical_plan/todo.md` and
`../01_pipeline/08_execution/todo.md` (one key table, one key-grouping path),
`../01_pipeline/04_canonicalizer/todo.md` and `../01_pipeline/05_optimizer/todo.md`
(shapes and formulations), `../03_expressivity/scope/todo.md` (the keyed-scope rename),
`../03_expressivity/explain/todo.md` (columns over a join),
`../07_query_diagnostics/infeasible/todo.md` (the `=` half, the NULL class label),
`../05_performance/04_model_construction/todo.md` (a helper's box per instance) and
`../06_issues/code_quality/todo.md` (the structural follow-ups).

## The full algorithm — phase 0

The rest of the deck's ANR proposal — variant signatures, reducers nested to any depth,
frame selectors — was planned phase by phase in a plan of its own, written and reviewed on
2026-10-27 and retired when its last phase shipped (its design notes moved into the
stages' records, its contract into `syntax_reference.md` §5.5–§5.6 and §7.1, its
capability matrix into `../03_expressivity/scope/done.md` §11a, its tests into
`../04_testing/scope/done.md`). Its sixteen decisions, settled on 2026-10-28, are D64–D79;
D80–D83 were settled as later phases needed them. Phase 0 shipped the same day and changed
no engine code:

- `test/decide/tests/_anr_oracle.py`, a reference evaluator for the whole planned
  language, and `_anr_cases.py`, the plan's fixtures with their hand-computed answers
  ([`../04_testing/scope/done.md`](../04_testing/scope/done.md)).
- `test_anr_oracle_selfcheck.py`: the evaluator reproduces the deck's constraints
  (pp. 35, 36, 97, 108, 112, 115, 119), agrees with exhaustive enumeration, gives today's
  answer on the 424 cells of the scope grid where today's rule holds and answers the 360
  it refuses, reads the two-level objective as a nested reducer, and matches DeciDB's
  feasibility on every fixed assignment of two small grids.
- Baselines for later phases: the golden corpus identical on both solvers, and the
  medium-tier timings in
  [`../05_performance/04_model_construction/done.md`](../05_performance/04_model_construction/done.md).
- Found on the way: a bound-side `count(e)` refused a NULL instead of counting the
  non-NULL values; fixed in phase 2.

## The full algorithm — phase 1: variant signatures

Shipped 2026-10-28. A scoped expression is one instance per class of its key and distinct
value of its **signature** — the tuple of what its body reads once per class — instead of
the determination error (D64; deck pp. 34–40; `syntax_reference.md` §7.1):

- **Fields** (D77): a maximal subtree with no decision and no reducer, by value, the data
  terms of one additive context summed into one so written associativity never changes
  the count; a decision by instance (its row, or its class); a reducer's `by` key.
  `DecideCanonicalizer::BuildConstraintSignatures` / `BuildObjectiveSignatures` read them
  off the canonical tree into `LogicalDecide::signatures` (`DecideSignature`, property
  221; `objective_signature`, 222), leaving out a field the key already holds, so a clause
  the old rule accepted keeps today's model.
- **Clauses**: a `per K:` wrapper carries `__signature_<i>__`; execution numbers the kept
  rows by variant (`BuildSignatureIds`) and the rows shape emits one row per variant at
  its first row. The aggregate shape and the easy MIN/MAX already cover every variant of a
  group with its tightest bound, `=` still naming two different values a contradiction.
- **A reducer's own `per`, and the colon form** (D65): a reducer whose body reads a value
  its key does not fix gets a scope of its own (`EntityScopeInfo::signature`) whose
  entities are the variants, so the existing de-duplication keeps one value per variant
  everywhere a qualifier is read. This is the trap P2 named: `sum(per D: D.cost * open +
  T.unit * ship)` counts a depot once per route. EXPLAIN ANALYZE prints the count under
  the clause (*"↳ sum(per D: ...): 3 values from 2 classes, one per distinct (T.unit,
  ship) within a class"*), and a `per` clause's (*"per t.g: 2 constraints from 1 class,
  ..."*). Plain EXPLAIN runs nothing, so it cannot count.
- **The objective** is produced once (D66): a second variant over its kept rows stops the
  query, naming the field — *"DECIDE objective: 'w' takes more than one value over the
  kept rows (3, 5), so the objective would be produced more than once; move it inside a
  reducer or fix it with when."*
- **DIAGNOSE** labels a variant by its class and its own values where its class has more
  than one (`a, 2`).
- **Retired**: the `DETERMINES` builders and both determination messages (the enum value
  stays, never built); `DecideScopeCheck`'s value, decision and texts. `_scope_oracle`
  reads variants off its own statements, and `_anr_oracle`'s fields were aligned to the
  engine's (a decision is a field by instance whatever its coefficient, D77).
- Tests: `test_anr_variants.py` (the phase-1 fixtures, model size, NULL, `=`, `<>`,
  EXPLAIN ANALYZE, DIAGNOSE); `test_scope_variants.py`, which replaced
  `test_scope_determination.py`; the 360 grid cells the old rule refused now answer,
  judged by the oracle; every pin of the old message became an answer or the objective's
  error. No "does not determine" remains in `test/` but the frame partition phrase of
  phase 4.
- Corrected in the plan: two classes with equal bodies are two model rows (nothing merges
  them, and nothing needs to); `rows_stand_with_their_class` stays sound, since a row
  stands with its variant's first row, which carries the same decisions and data.

## The full algorithm — phase 2: nested reducers

Shipped 2026-10-28. A reducer nests in another to any depth, with `when`, `per` and `by`
at every level, on either side of a constraint and in the objective
(`syntax_reference.md` §5.5; deck pp. 16–32, 79–97, 111–119):

- **The path rule.** A nested reducer reads, at the row it stands for, the kept rows that
  every enclosing reducer's own `when` keeps and that agree with that row on every
  enclosing `by` key and its own, its path key. `DecideReducerPath`
  (`planner/decide/decide_reducer_path.hpp`) states it once; the canonicalizer reads it
  for a nested reducer's D8 check (named by its path group, *"max(when c: x) has no rows
  for t.g = 'a', t.h = 'q'"*) and the rows its variants are counted over, and the value
  rewrite for its helper's key and rows.
- **Binder.** The nesting gate is gone; a nested reducer's body is held to a reducer's
  rule. Refused (D74): `norm` inside a reducer, and a reducer over data alone added
  inside one over decisions.
- **Constraints.** A decision reducer holding a reducer is the VALUE shape. The value
  rewrite recurses innermost first: every reducer of a nest is a helper decision keyed by
  its path (a new union of keys is a `group_keys` entry the optimizer adds), its members
  read the inner helpers through their entity mappings, and a MIN/MAX is pinned on the
  side its readers push it through SUM and AVG levels, on both under another MIN or MAX
  (D73). F1 needs no Big-M: twelve columns, eight rows for the SUM inner, ten for the MAX.
  A square or product of decisions inside nested reducers is refused by name (D74).
- **Bounds.** `DeriveReducerMemberBounds`, the backward half of
  `DeriveReducerValueBounds`: a helper's box bounds the helpers it is defined from
  (through a SUM/AVG equality, through a MIN/MAX envelope), so a bound written only at
  the root reaches a nested closing link (`per t.g: sum(max(x) by (t.g, t.h)) by (t.g) =
  10` answers on HiGHS with MIN/MAX lowered). Only helper columns are tightened, when
  every instance a helper defines is bounded; one box per helper stays a limit
  (`../01_pipeline/05_optimizer/todo.md` §2). A helper's columns are named by its reducer
  with its `by`, so the remedy of *"requires a finite bound on 'sum(x) by (t.g, t.h)'"*
  can be written.
- **The bound side.** A nest over data alone is evaluated innermost first, each inner
  reducer over its path groups and read as a chunk column (F14). `count(e)` counts the
  non-NULL values, 0 for a group that has none.
- **The objective** (D75). `outer(per k: inner(e) by (k))` is a nested reducer: the
  canonicalizer tags each objective reducer that holds one, the value rewrite makes the
  nest helpers pushed by the sense, and the objective reads the outer helper once. Its
  four refusals lifted (a `when` in either reducer, a `by` other than the `per` key, an
  outer `avg`), and it nests deeper and beside other terms. `sum([when c] per K:
  sum|avg(e) by (G))` with `K` in `G` is the objective's own sum instead, each group
  once and an inner AVG dividing by its group's size (`DecideTerm::avg_group_key`), which
  keeps a square in `e` a convex QP; a MIN/MAX of a square inside nested reducers is
  refused. Deleted with the special path: `BindTwoLevelObjective`,
  `IsTwoLevelObjective`, the two-level branch of `RewriteMinMaxObjectiveTree`, PATH B of
  `LinearizeMinMaxObjective`, the `per_*` metadata, `DecideObjective::per_columns`,
  the objective's group ids, `RenderTwoLevelObjective` / `RenderTwoLevelInner`, the bound
  `PER_CONSTRAINT_TAG` wrapper and its predicates (the tag stays the removed spelling's
  parsed marker), and the `EXTREMUM_PER_GROUP` accounts.
- **Answers that changed.** The old path read a MIN/MAX of a square as the sum of the
  squares, and an inner AVG under a budget the groups share without its 1/n per group;
  both were wrong where the rows were coupled, and the tests that pinned them passed
  only because theirs were not. The first is refused now, the second exact. The
  removed-spelling messages for `... PER k` name the nested spellings that now work (an
  outer `AVG` kept, an inner colon form, a nest under a factor).
- **Golden corpus**: entries 91 and 92 (the two nested objectives) changed model -- a
  helper per group and one over them, in place of the objective's extremum column -- on
  both solvers; on Gurobi each answers with another assignment at the same optimum (6 and
  0). Re-captured; every other entry identical.
- **A bound on a lone helper.** Rewritten, `per t.g: sum(max(x) by (t.g, t.h)) by (t.g)
  >= 3` is `h >= 3` on the outer helper, which bound absorption folds into its box;
  execution freed every helper's floor after reading the box, so a `>=` or `=` bound on a
  nest was dropped and answered wrongly (all zeros under MINIMIZE). Found by probing
  after the first report; `BuildSolverInput` now frees a helper's floor only where none
  was written.
- **Found by the review of 2026-10-29** (a random differential test of 6,000 queries —
  nests, variants, keyed and query-wide decisions, joins, both solvers and the lowered
  path — against `_anr_oracle`):
  - A feasible nest over negative terms read as infeasible: `per t.g: sum(sum(t.w * x)
    by (t.g, t.h)) by (t.g) <= -4` was refused with *"cannot hold because ... is never
    negative"*. The static domain check of execution, meant for an ABS auxiliary, took a
    helper's default floor 0 for its domain; a helper is skipped now
    (`../01_pipeline/08_execution/done.md`).
  - A feasible nest read as infeasible by rounding: through an AVG's 1/n the backward pass
    put a helper's floor at 8.000000000000002 under a top of 8, and the model builder took
    the box for empty. A box inverted by rounding alone is closed onto its top there,
    whatever derived it (`../01_pipeline/06_model_formulation/done.md` §8).
  - An infeasible nest was an internal error (*"the ABS and helper decision boxes did not
    settle"*): contradicting clauses make a helper box cross, and each pass pushed its
    ends further apart. The alternation stops at the first crossing, and the query is
    infeasible, DIAGNOSE included.
  - `MAXIMIZE max(avg(t.w * x) by (t.g))` was an internal error on HiGHS, which rejected
    its own optimum over the helpers' Big-M rows; it is solved again with integrality
    held tighter (`../01_pipeline/07_solver/done.md`).
  - A D8 message dropped the `by` of a reducer nested in the one it names: it reads
    `sum(when c: max(x) by (t.h)) has no rows for ...` now.
  - Three wrong answers where a rewrite run before the value rewrite met a nest, each
    refused now (D74): `abs` over a reducer inside a reducer whose `when`, `per` or `by`
    chooses its rows (the ABS rows lost them; `sum(abs(x - avg(x)))` still answers), a
    reducer inside `norm` (which becomes ABS, or an L0 link whose Big-M preceded the
    helper's box), and a reducer times a decision inside a reducer (the BOOL product read
    `max(x) by (g)` as `x`).
  - The objective summed an inlined `sum(per K: avg(e) by (G))` and another AVG of the
    same decision into one like term with one divisor (`TermsAreLike` reads
    `avg_group_key` now).
  - `sum(t.w * max(x)) = 9` over weights 2 and 1 never settled its helper's box (an
    internal error): the backward pass sums a group's occurrences of one column first, and
    a box still closing in after the pass budget stands as it is.
  - `count` added inside a reducer over decisions escaped the D74 refusal.
- Tests: `test_anr_nested.py` (the phase-2 fixtures on both solvers, F1's model, a
  768-cell grid at depth 2 and 3, a constant bound on a nest under each sense, members,
  bounds through a nest, the refusals, and the review's cases: an infeasible nest, a
  negative bound over negative terms, a MAX objective over averages on HiGHS, the inner
  `by` in a D8 message);
  `test_scope_objective.py`'s refusals, the nesting pins of `test_scope_bare_clauses.py`
  and `test_scope_reducer_by.py`, and deck example 9 in a constraint became answers
  judged by `_anr_oracle`; the two-level EXPLAIN, box and quadratic tests follow the
  helper formulation.
- Beyond the plan's text: the outermost objective reducer is a helper too rather than
  riding the objective's SUM/MIN/MAX/AVG paths (one path; the objective sum of group sums
  is the exception above); a path key is a group key rather than two new link fields;
  `_scope_oracle.ScopedModel` still states its own `Nested` form rather than delegating
  to `_anr_oracle` (plan §6.2).

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
  bound onto the nested objective (`PER_CONSTRAINT_TAG`, `DecideObjective::per_columns`)
  until phase 2 made it a nested reducer (D75).
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
  `test_scope_variants.py` (variant signatures, phase 1), `test_scope_objective.py`
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
  automatic L0 bound prints as `M`; the two-level objective's rewritten row read
  `when c per k: inner(e) by (k)`, its key as written (`RenderDecidePerKey`), until
  phase 2; functions
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
  in front. An outer `avg` in the two-level objective was refused (until phase 2, D75).
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
  `COLON_FORM_TAG`, so it echoes as written; a body that reads a value `D` does not fix
  is one value per variant (phase 1, D65), and a keyed decision inside it answers. `by` is
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
  in the serialization JSON, `serialize_*.cpp` regenerated. `PER_CONSTRAINT_TAG` wrapped
  only the two-level objective from then (since phase 2 it is only the removed spelling's
  parsed marker); a constraint's `per K:` is `PER_CLASS_TAG`.
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
- Under an explicit `per`, a bound that varies within a class was the determination
  error until phase 1, which makes each value a variant (D64); with no `per`,
  `sum(x) <= cap` still takes the tightest bound (S3).
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
  an error (since phase 1: the objective would be produced more than once, D66); a
  query-wide decision
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
  (`../01_pipeline/02_binder/todo.md` §1).
- The colon form's bind-time row-invariance check was deleted, not moved: the
  decision side already has the parsed-tree check, so `x <= SUM(D: 2)` answers like
  `x <= sum(per D: 2)`.
- The renames (D46) and the structural follow-ups (D47) are a separate change; the
  two key-grouping paths stay unmerged (`../01_pipeline/08_execution/todo.md` §1).

## The full algorithm — phase 3: frame syntax

Shipped 2026-10-29. Every frame selector of `syntax_reference.md` §5.6 (deck pp. 43–56)
parses, binds, plans, round-trips and prints; a query holding one stops before
formulation, from one place, with *"DECIDE: frame selectors are not supported yet:
at(previous: x) over (t.period)"*.

- **Parser** (`../01_pipeline/01_parser/done.md` §1–§3, §5). Three DECIDE-only tokens,
  `AT_DECIDE`, `OVER_DECIDE` and `WITHIN_DECIDE`, emitted only at the clause's own level:
  new lexer state counts parentheses and the SQL subqueries written in the clause, so a
  window inside one keeps its `OVER`. A window at the clause's level parses as before
  (`over_clause` takes `OVER_DECIDE`). `previous`, `every` and `cyclic` are identifiers
  checked by their text, not keywords. No new grammar conflict. A frame is a parsed
  class of its own, `DecideFrameExpression`, printed with its defaults left out (D32);
  `k` and `d` of 0 are parse errors (D71).
- **Binder** (`../01_pipeline/02_binder/done.md` §4a, "Frame selectors"). A frame binds to
  an aggregate over its payload, `first(e)` for a point and its reducer for a range,
  tagged `__frame_<i>__`; its settings, order key, `within` columns and fallback are
  `LogicalDecide::frames[i]`. Refused by name (D74): a frame in a `when`, `per`, `by`,
  `within` or order key; a decision in a frame's keys; a reducer or a frame in a payload
  or fallback; a frame term times or over a decision, or a square of one over decisions;
  a range reducer other than SUM, AVG, MIN, MAX and COUNT; a fallback with no implicit
  cast to the payload's type.
- **Logical plan** (`../01_pipeline/03_logical_plan/done.md` §2, §4). `frames` is
  property 220. Subqueries in a frame's order key and fallback are planned, and — closing
  the gap the plan named — the subqueries of the `by` keys and frames carry the same
  correlation tags as the clause's.
- **Choices beyond the plan's text.** A point frame binds to `first(e)`, not a
  `FRAME_TAG` wrapper over the payload: a wrapper's payload would read as a term of the
  anchor row to every layer that opens a sum, while an aggregate is already opaque to
  them. The gate stands in `CreatePlan` just before canonicalization, the first layer
  that would read a frame, so `EXPLAIN`, `DIAGNOSE` and `CREATE VIEW` (each plans) refuse
  too, no plan holds a frame yet, and the plan's "EXPLAIN of the plan before execution"
  and the bound plan's round trip of `frames` wait for phase 4. The `FRAME_PARTITION`
  check (D69) moved to phase 4: it is a run-time check the canonicalizer builds and
  execution runs, and nothing past the gate runs yet. No keyword is added for
  `previous`, `cyclic` or `every`; they are read whatever their case. A fallback is any
  number (`else true`, `else 0.5`, `else c`), in its own type. A frame's payload and
  fallback are linear in the decisions.
- **Found by the review of 2026-10-29** and fixed: subqueries written in a clause that
  start with `FROM`, `VALUES`, `TABLE`, `WITH` or `PIVOT` lexed `at`, `over` and `within`
  as a frame's (they keep their SQL meaning now); a decision named `at` could not be
  declared (the frame words are the body's); a named window in a clause, upper-case frame
  words, and a window or frame in an order key before a `within` were syntax errors; a
  range over `construct_array` or `if` was an internal error, and a frame re-bound by an
  outer query an unnamed one; a frame alone as a constraint named an internal class; and
  the gate's first place let the canonicalizer refuse some frame clauses first, in
  reducer wording.
- Tests: `test/decidb/test_frame_parser.test` (every spelling to the gate, the echo
  through a view, the exact parse and bind errors, ordinary windows),
  `test_decidb_plan_serialization.cpp` (parsed and bound round trips of every form),
  `test_anr_frames.py::TestSyntax` (every frame fixture of `_anr_cases` binds and stops at
  the gate; the refusals; a window in a subquery of the clause answers).

## The full algorithm — phase 4: point selectors

Shipped 2026-10-30. A point frame `at(F [else v]: e) over (S [desc] [cyclic] [within K])`
is formulated wherever phase 4 places it (`syntax_reference.md` §5.6; deck pp. 43–56,
73–75, 99–108); a range still stops at the gate, now its own (*"DECIDE: a range frame
selector is not supported yet: ..."*), until phase 5. The design is the plan's phase-4
section; in short, a frame is a tuple term read at another row, so every layer above
execution keeps it opaque, and execution turns it into data columns and decision reads.

- **Binder** (`../01_pipeline/02_binder/done.md` §4a). `else NULL` written is no fallback
  (D80; the transformer drops the literal, the binder a fallback that folds to NULL). A
  point's marker takes the common type of payload and fallback (`else 0.5` over a whole
  payload). Refused: a payload or fallback not linear in the decisions (`abs(x - 3)`
  inside), whose rewrite rows would stand at the anchor while the frame reads elsewhere;
  a fallback that reads a decision beside a payload that reads none, which no layer below
  would see (the marker's children are the payload's).
- **Plan** (`../01_pipeline/03_logical_plan/done.md` §4). `DecideFrameSpec` gains `rows`,
  `scope_key`, `source_clause_id`, `reduced`, `timeline_names`, `order_name` (ids
  113–118); `DecideScopeCheckKind::FRAME_PARTITION` with `DecideScopeCheck::frame` (110);
  `DecideSignatureFieldKind::FRAME` with `DecideSignatureField::frame` (105).
- **Canonicalizer** (`../01_pipeline/04_canonicalizer/done.md`). A frame's marker is no
  reducer (`ContainsReducer`, `TryMatchScaledAggregate`): a frame over decisions sits
  left as a tuple term, one over data right; it decides no shape. `PlaceFrames` reads
  each frame's timeline rows off the path rule, builds the D69 check of a frame a `per`
  reads directly unless the key holds its `within`, and refused a frame that may select no
  position inside a reducer over decisions (phase 6 lifts it). A frame is a signature field
  by its keys (D76), never by the data it reads.
- **Optimizer** (`../01_pipeline/05_optimizer/done.md` §1a). The rewrites carry the marker
  as a term (the value rewrite lifts no frame). The linear form lowers it: a payload term
  `c·x` becomes `READ(c)·x` read at the selected row (`DecideTerm::frame_read`), a
  fallback's `c'·y` becomes `FALLBACK(c')·y` at the anchor, a frame over data one `VALUE`
  column of its own type (a DATE or BOOLEAN payload too), and one inside a bound-side
  reducer adds its `PRESENT` column to the reducer's own `when`
  (`DecidePreparedModel::frame_columns`). Like terms compare `frame_read`, so
  `x - at(previous: x)` keeps both. A frame over decisions inside, or beside, a composed
  MIN/MAX was refused (phase 6 lifts the first). Bound absorption treats a clause whose root frame
  may select nothing as it treats a `when`: its bounds hold only where the frame selects,
  so none becomes a column bound (found by the fuzzer: `abs(x - at(previous: x) over
  (...)) <= -1` with every instance skipped was called infeasible).
- **Formulation** (`../01_pipeline/06_model_formulation/done.md` §1). A frame read is a
  variable of scope `FRAME_READ` (`SolverInput::frame_reads`): `VarIndexer::Get` returns
  its base's column at the selected row; it owns no column, and its type and box are its
  base's, re-copied after implied bounds and after a diagnosis widens the boxes.
- **Execution** (`../01_pipeline/08_execution/done.md`). `BuildFrames` builds each frame's
  timelines (DuckDB sort keys, `default_null_order`, D68), selections and columns, appended
  to the materialized input, a payload computed on each selected peer's first row only and
  a fallback on the anchors that reach it only. `FRAME_PARTITION` runs with the scope
  checks, and the peer error (D67) after them. A comparison whose root frame selects
  nothing skips that instance (`FramesPresent`, by its clause id through
  `DecideFrameSpec::SkipsInstancesOf`, so a rewrite's rows of it too) while its
  reducers still read every kept row; under the tightest-bound collapse an absent anchor
  bounds nothing and a group with none has no row (D79); a bound-side reducer left with
  no member is absent. The objective's root frame selecting nothing is an error. Readback
  reads the input's own columns.
- **Choices beyond the plan's text.** A point frame inside a reducer operand is read in
  place too, not through a helper per anchor row (D72 as shipped): `sum(at(previous else
  0: x))` is a sum of redirected terms. The redirect lives in `VarIndexer::Get`, where
  every column resolution already passes, rather than at the model builder's thirty call
  sites. The peer error is raised after the scope checks, so a spanned timeline (D69) is
  reported first, as the oracle reports it. A `BETWEEN` is its two comparisons: a frame
  absent in one bound skips that half alone.
- Tests: `test_anr_frames.py` (`TestSyntax` on phase-4 terms; `TestPoint`: the 21 phase-4
  fixtures of `_anr_cases` and F2's model pinned at six columns and one row per instance;
  `TestPointDeck`: deck examples 1, 4, 5 and the DeQL comparison as written;
  `TestPointShapes`, `TestPointErrors`), `test_frame_parser.test` (answers, the gate on
  ranges, the new refusals), `test_decidb_plan_serialization.cpp` (a placed frame, its
  check and its field round-trip). A differential fuzzer against `_anr_oracle` (random
  timelines with peers and NULL keys, every placement above, three solver settings) ran
  2,000 cases: no wrong answer and no internal error; one absorption bug (above), and
  differences only in which of two errors a query reports first — the engine runs every
  scope check before the peer check, and a domain contradiction before either, where the
  oracle goes clause by clause. A second run of 1,000 cases on the final binary found one
  more disagreement: the peer check demanded one decision instance where the coefficient
  was 0 at both rows. D67 now compares payloads as linear forms, as the oracle does
  (`test_peers_with_one_linear_form_select_once`). A third run of 1,000 found only
  error-order differences.
- **Architecture review** (a `decidb-architect` pass over the phase-4 diff). Fixed: an L0
  `norm` over a decision frame was an internal error (its Big-M named a frame read by the
  plan's decision table; `SolverInput::BaseVariable` now maps a read to its base); a frame
  over data of a DATE payload was a cast error and one with `else true` an internal error
  (`READ + FALLBACK` joined them by arithmetic and a 0 default; one `VALUE` column of the
  payload's type replaces it); a decision frame beside MAX in the objective named the
  internal `first` aggregate (now refused as itself); the skip test was written three
  times, and a comparison with no id would have read the objective's frames (now
  `SkipsInstancesOf` and `MayLeaveObjectiveUnset`, and placement holds every
  constraint's frame to an id); a lone negated frame took a reducer's spelling. Guards
  added: stage 06's composed MIN/MAX refuses a frame read internally (it reads terms at
  their own rows), a frame marker reaching execution's reducer collector is internal,
  and the frame columns must follow exactly the input's. Kept, with the reason: frame
  reads are numbered by execution, not by stage 05, since model variables are the
  formulation contract's (`ReadVariable` is the one place a term's variable is read, at
  all four evaluation sites); the input is widened by moving segments into the emptied
  collection rather than by holding it through a pointer; a sort key is a short string
  for the usual order types, held inline.

## The full algorithm — phase 5: range selectors

Shipped 2026-10-30. A range `agg(from F1 to F2 [every d] [else v | all]: e) over (S [desc]
[cyclic] [within K])` is formulated wherever a point is (`syntax_reference.md` §5.6; deck
pp. 43–56), with `sum`, `avg`, `min`, `max` and, over data, `count`; the gate that refused
it is gone. The design is the plan's phase-5 section: over data a range is a value of the
row, over decisions a helper decision per **window** — the range several anchors share.

- **Binder** (`../01_pipeline/02_binder/done.md` §4a). A range's payload and `else` take
  one type and its reducer is bound again over it (`else 0.5` over INTEGERs sums
  decimals); its reducer's name is recorded; an AVG range is no AVG the `<>` hoist may
  multiply through, so under `<>` it is refused as fractional.
- **Plan** (`../01_pipeline/03_logical_plan/done.md` §4). `DecideFrameSpec::reducer` (id
  119). `MaySelectNothing` covers ranges, exact per pair of ends (`MayBeEmpty`,
  `MayMissEveryPosition`): every layer that skips an instance, drops a member, refuses a
  shape or checks the objective reads it, and execution holds every map to it.
- **Canonicalizer** (`../01_pipeline/04_canonicalizer/done.md`). Nothing new but the
  remedy of the refusal phase 6 lifted, worded by what makes the frame read nothing
  (`DecideFrameAbsenceRemedy`, which the objective's error still reads).
- **Optimizer** (`../01_pipeline/05_optimizer/done.md`, "Frame selectors").
  `RewriteFrameRanges`, first among the rewrites: each range over decisions becomes a
  helper decision with one instance per window, its link naming the frame, its MIN/MAX
  sides read off the way its comparison pushes it (D73), so every later rewrite — ABS,
  `norm`, `<>`, MIN/MAX, the value rewrite — reads an ordinary keyed decision and the
  canonical shape stands. A range over data is one `REDUCED` column; a range's `else` has
  `FALLBACK` columns, read by the helper's definition and by the signature.
- **Formulation** (`../01_pipeline/06_model_formulation/done.md`, "Helper decisions of the
  value rewrite"). A helper's members arrive listed (`ReducerValueMembers`: per instance,
  its members, each a constant and terms read at rows), since a range's windows share rows
  where a `by` key's groups partition them; the three passes over links read that one form.
- **Execution** (`../01_pipeline/08_execution/done.md` §3a). The frame map lists each
  range's windows — one per timeline when neither end depends on the anchor, else one per
  position, split at an anchor that reaches an `else` reading its own row — and each
  window's rows with visit counts and missing positions, in O(values): a position off a
  timeline that does not wrap is counted, never visited. `decide_frame_limit` (a new
  setting, default 10,000,000 values) is checked before the work, an interrupt polled per
  position. A range over data runs its own bound aggregate through the aggregate's state
  functions over each window's values; a range over decisions' helper takes the windows as
  its entities and its members from them. Absence is per anchor (`DecideFrameMap::absent`),
  read by instance skipping, `PRESENT`, and the objective's check.
- **Choices beyond the plan's text.** Helpers are keyed per window, not per anchor row as
  §4.5 step 3 said, so `x <= sum(from first to last: x) over (t.period)` over n rows is one
  definition of n terms rather than n of them; the stage-06 link contract changed to listed
  members for every link, not a second form beside rows; the limit counts the values read
  (ties in the order key make one position many rows), not the positions requested; a
  range's helper replaces the marker before any rewrite, rather than the value rewrite
  lifting it (which runs on VALUE-shape clauses only, so a range at a ROWS clause's root
  would have reached flattening). An AVG range under `<>` is refused as fractional, where
  a plain AVG is multiplied through. `count` over decisions keeps the binder's refusal.
- **Found while building it** and fixed: an AVG range beside a decision under `<>` reached
  the model builder as an internal error (the binder's `<>` hoist read the marker as an AVG
  reducer; now refused as fractional, `test_an_average_range_under_not_equal_is_fractional`);
  HiGHS's MIP presolve called a point optimal on a model with no integer point and reported
  a solve error (now solved once more without presolve, `../01_pipeline/07_solver/done.md`).
- **Architecture review** (a `decidb-architect` pass over the phase-5 diff). Fixed, each with
  a test in `TestRangeReview`: a range's helper was whole-numbered by its payload alone, so
  `else 0.5` called a feasible query infeasible and stepped `>= 0.5` to `>= 1`, and a strict
  bound on `sum(...: x + 0.5)` reached the model builder (a frame's value is now whole only
  when the type payload and `else` share is, one test for the binder and the helper); an
  `else` of another type than the reducer's argument (`CAST(0.5 AS FLOAT)`) was an internal
  error; `count ... all` that cannot miss a position was refused (`MayMissAPosition` is exact
  per pair of ends, and a C++ test enumerates every position against each predicate); a
  window `all` makes absent evaluated its payload; the windows of a parted position each read
  their list but were charged once against the limit; `count` read a NULL `else` as an
  error (it skips it, as a NULL payload, and `_anr_oracle` with it); the range rewrite
  duplicated the value rewrite's walker and helper allocation (now shared,
  `CollectDecisionSites`, `ExtremumSides`, `AddValueHelper`, the rewrite in a file of its
  own), the signature found a range's `else` columns by a side effect (now
  `frame_fallback_columns`), and execution's two readers of a window repeated its walk (now
  the map's). Refactoring the shared allocation exposed one more bug of this phase: the value
  rewrite matched a reducer's occurrences to links by position, which a range's link, made
  first, displaced, so an ABS envelope's copy of a reducer read the range's helper and an
  infeasible query answered (`Helpers::links`). Measured against the last commit on 1,000,000
  rows of a helper per `by` group, the listed members cost no memory (1.81–1.93 GB against
  1.94–2.01 GB) and time (2.1 s against 2.3 s); the models dump identical. Kept: a range's
  reducer as its SQL name, beside the links' `agg_name`; cumulative ranges' n² values and a
  parted position's repeated reads, both `../05_performance/04_model_construction/todo.md`.
- Tests: `test_anr_frames.py` (`TestRange`: the ten phase-5 fixtures of `_anr_cases`, every
  pair of six ends under both steps, each policy, open and cyclic — 432 cases —, each
  reducer over data and over decisions in every push direction, the helpers per window
  pinned in the model dump, composition with ABS, `<>`, a reducer around the range, a data
  factor and a point frame, the objective, a keyed payload and a decision `else`, variants
  under `per`, laziness, NULL and `count`, SQL types of a range over data; `TestRangeDeck`:
  deck examples 2 and 6 under each policy; `TestRangeErrors`: `count` over decisions, the objective, `decide_frame_limit`, EXPLAIN's
  helper lines, DIAGNOSE), `test_frame_parser.test` (the former gate cases answer),
  `test_decidb_plan_serialization.cpp` (a range's fields round-trip). A differential fuzzer
  against `_anr_oracle` (scratchpad `fuzz/fuzz_ranges.py`: random ranges over data and
  decisions beside point frames, at a comparison's root, on its bound, under ABS, inside
  decision and bound-side reducers, under `when` and `per`, in the objective; three solver
  settings) ran 4,300 cases, ranges in 93% of them, 300 over timelines of up to 18 rows and
  the last 1,000, after the review, with fractional payloads and `else` values: no wrong
  answer, no internal error; its only differences are which of two errors a query
  reports first (the engine runs every scope check, and a domain contradiction, before the
  peer check).

## The full algorithm — phase 6: composition

Shipped 2026-10-30. A frame that may select no position inside a reducer over decisions
drops that member (deck p. 50, example 3), under every shape a reducer takes -- rows, easy
and hard MIN/MAX, composed MIN/MAX, nests, the objective -- and under `abs` and `norm` (D81,
D82); a frame over decisions is formulated inside a composed MIN/MAX. The design is the
plan's phase-6 section: such a reducer is a helper decision whose members execution lists
where the frame selects, so members are dropped in one place.

- **Canonicalizer** (`../01_pipeline/04_canonicalizer/done.md`). Before it runs, the planner
  tags every reducer with the frames that may drop its members (`TagDecideMemberFrames`,
  `__member_frame_<f>__`, one definition: `DecideMemberFrames`); a reducer over decisions so
  tagged takes the VALUE shape in a constraint and the value rewrite's tag in the objective
  (`DropsMembers`). Placement refuses a reducer that may be absent under `abs` inside
  another reducer (`FirstAbsenceFrame`). The refusal of a frame that may select nothing
  inside a reducer over decisions is gone.
- **Optimizer** (`../01_pipeline/05_optimizer/done.md`). The rows the ABS, Boolean-product
  and L0 rewrites add for a construct inside a reducer over decisions are its **member
  rows** (`MarkMemberRow`: `__member_row__` and the reducer's member-frame tags); an ABS's
  read the rows the reducer's path keeps, every enclosing reducer's own `when` included. A
  helper's link records at plan time, as a frame does, what makes it absent and where it is
  read (`ReducerValueLink::member_frames`, `absence_frame`, `read_at_root`; `MayBeAbsent`,
  `SkipsInstancesOf`, `MayLeaveObjectiveUnset`). Bound absorption leaves a clause that may
  skip an instance as rows (`ClauseMaySkipInstances`). `InlineSumOfGroupSums` keeps such a
  reducer a helper. The linear form joins each member frame's PRESENT column to the link's
  own `when` and to a member row's (`DecideConstraint::defines_members`), and the composed
  refusal (RefuseComposedFrameRead) is gone; it holds the routing to two internal checks (no
  frame that may select nothing, and no helper that may be absent, inside a reducer summed
  in place).
- **Formulation** (`../01_pipeline/06_model_formulation/done.md`). Composed MIN/MAX reads
  `ComposedMinMaxTermData::variables`, the variable execution numbers each term by, so a
  frame read lands on its selected row; a helper instance with no listed group has no
  definition.
- **Execution** (`../01_pipeline/08_execution/done.md`). Helpers are evaluated before the
  clauses, innermost first: a member stands where its member frames select and every helper
  it reads is present, and each helper's present instances are recorded
  (`DecideGlobalSinkState::helper_present`), held to the plan (an absent instance of a link
  that cannot be absent is internal). A clause's instance rows skip an instance where a
  helper the plan marks (`SkipsInstancesOf`) is absent (`InstancesPresent`), never its member
  rows; the objective reading one absent (`MayLeaveObjectiveUnset`) is an error. The
  interrupt is polled while members are listed.
- **Defects found while designing and fuzzing it**, fixed, each with a test in
  `test_anr_composition.py` (`TestDefects`), or `test_error_binder.py` for the last: a frame at a comparison's root that selected nothing skipped every row of
  its clause at that anchor, the rows an ABS or a Boolean product inside one of the clause's
  reducers adds to define its members included, so `sum(abs(x - 3)) + at(previous: x) over
  (t.period) <= 4` answered x = (4, 3, 3), which breaks the second period's instance (phase
  4); and the rows of an ABS inside a reducer were read on every row the clause keeps, not
  only the reducer's own, so `sum(when t.w is not null: abs(x - t.w)) <= 3` raised a NULL
  error the language rules out (older than the ANR work). Found by the fuzzer: an ABS
  pinned exactly read its argument short -- a frame's `else` read as data is a term of no
  decision, which the native row and the Big-M left out, so `abs(x - at(previous else 50:
  x) over (t.period)) >= 1` was called infeasible on both arms, and where a frame selects
  the row itself (`x - at(last: x)`) the native row named one column twice, which Gurobi
  refused (phase 4; `../01_pipeline/06_model_formulation/done.md`, "The argument whole");
  and a comparison whose reducer side reads no decision under arithmetic (`sum(t.v) by
  (t.g) + 1 <= 30`) reached the linear form as an internal error (older; now the binder's
  *"... reads no DECIDE variable"*).
- **Choices beyond the plan's text.** A reducer whose members a frame may drop is a helper
  even where the aggregate shape would have summed it in place (deck example 3 costs a
  helper and a definition per product, `TestMembersDropped::test_a_helper_holds_each_group`)
  -- the user's call of 2026-10-30, for one place that drops members; phase 7 writes such
  helpers back in where that pays (D83). A member row carries its innermost reducer's member frames even where
  the frame stands beside the construct (`sum(abs(x - 3) + at(previous: t.v))`): its
  auxiliary is then read by no member there, and leaving the rows out is exact. A decision
  or frame beside a MIN or MAX in the objective stays refused, as for any decision there
  (§10.2), not as a frame's limit. The two new refusals carry the frame's remedy.
- **Architecture review** (a `decidb-architect` pass over the phase-6 diff, no wrong answer
  found). Adopted: a helper's link carries its absence and where it is read as plan-time
  facts, as a frame does (`absence_frame`, `read_at_root`, `SkipsInstancesOf`,
  `MayLeaveObjectiveUnset`), where execution had rebuilt them from the rows' terms (and the
  objective's check took a row with no clause id for one serving the objective) -- so
  inlining a helper (phase 7) cannot silently stop a skip; member frames are tagged once,
  before canonicalization, by one definition, where placement had tagged them by a second
  walk the shape decision could not see; member rows are marked by one helper off the
  reducer path (`MarkMemberRow`, `DecideReducerPath::innermost`), where four rewrites built
  the tag string by hand; a data reducer's PRESENT conditions read the same tags
  (`JoinPresence`); a generic `ParseDecideIndexTags`; execution's helper lookups by table.
  Kept, with the reason: a Boolean product's rows inside a reducer keep no `when` (they read
  decisions alone, and scoping them would change model dumps for nothing); which stage owns
  phase 7's inlining is phase 7's call, the review's advice recorded there (keep the link as
  what lists members; inline at stage 06; one inliner beside `InlineSumOfGroupSums`).
- Tests: `test_anr_composition.py` (`TestMembersDropped`: every reducer shape over a member
  frame on both solvers, with a product of one period to make a reducer absent, ranges that
  read nothing, nests, the helper pinned in the model dump; `TestUnderAbs`: sums and maxima
  of `abs`, `norm` of order 1, `inf` and 0, the smoothing objective, the two refusals;
  `TestObjective`: sum, avg, min, nests, composed MIN/MAX with a frame, the objective error;
  `TestComposition`: §6.7 of the plan -- a frame inside a nested operand, a nest beside a
  frame, ABS over a range sum, `norm`, composed MIN/MAX with frames, `per K` with a `by`
  reducer, variants with frames on F3's depots, duplicate joined rows; `TestDefects`),
  `test_anr_diagnostics.py` (§6.8: F17 judged by the oracle, the structural rows never
  offered for repair, the expanded groups of a member helper, the unbounded remedy through
  a helper, F1 with its limits lowered, an atomic DROP that keeps another clause's helper,
  EXPLAIN's accounts), `test_frame_parser.test` (the former refusal answers). A
  differential fuzzer against `_anr_oracle` (scratchpad `fuzz/fuzz_compose.py`: members over
  points and ranges that may select nothing, under `abs`, nested, beside a decision, under
  `abs` at the root, in a composed MIN/MAX and in the objective; three solver settings) ran
  10,000 cases. The first 2,000 found the defects above beside two of this phase's own (the
  spare entity of a helper's scope counted as an absent instance, so a range read inside a
  reducer summed in place tripped the guard; a helper read only by the objective's member
  rows was taken for one its root reads, an error where the oracle answers); the next 4,000
  the exact ABS's constant terms; the last 4,000, on the final binary, no wrong answer and
  no internal error. What remains are which of two errors a query reports first (a domain
  contradiction before a data error), an existing refusal of an unbounded member under a
  hard MIN, and three models HiGHS 1.11.0's presolve answers wrongly -- a suboptimal point
  called optimal, two feasible models called infeasible -- which HiGHS 1.12.0 solves right
  (`../01_pipeline/07_solver/todo.md`).

## The full algorithm — phase 7: inlining, performance, closure

Shipped 2026-10-30. A helper decision of the value rewrite is written where it is read --
into the rows, the linear objective or another helper's members that read it -- when that
adds no nonzero to the model (D83), as the deck lowers its own example (pp. 112–119); a
MIN or MAX group of one member is that member. Q4's model construction is back under the
pre-ANR baseline, and the ANR workloads grow linearly in the input
(`../05_performance/04_model_construction/done.md`). The plan is retired.

- **Measured first.** On `x >= sum(from 2 previous to previous else 0: x) over (t.p within
  t.g) - t.w` over 200,000 rows, a helper per window made 400,000 columns and 400,000 rows,
  where writing each window into its anchor's row makes 200,000 of each, and Gurobi's solve
  falls from 0.82 s to 0.35 s on the same answer: presolve does not make a helper free.
  Q4's regression since phase 2 (`max(per l_returnflag: sum(keep * l_extendedprice) by
  (l_returnflag))`, 622 ms of model construction before ANR, 790 ms at phase 6) was not its
  model -- three group sums cost three columns -- but two passes doing needless work on a
  helper of half a million members: the backward bound pass, 122 ms though Q4 has no ABS,
  and the definitions, 117 ms, a hash map per member.
- **Linear form** (`../01_pipeline/05_optimizer/done.md`). It decides once, over everything
  it flattened, where each helper may be written in (`MarkInlinableHelpers`,
  `ReducerValueLink::inlining`): into its readers, when each is a linear row of a plain
  clause or the linear objective; or into one outer link's members (`inline_outer`), an
  outer MIN or MAX pinned on one side; and never with a clause folded into its box.
- **Formulation** (`../01_pipeline/06_model_formulation/done.md`, "Inlining a helper").
  `InlineReducerValueLinks`, at assembly before the column space is laid out, counts each
  instance's reads where the plan says it is read -- holding every read to the plan -- and,
  where inlining adds no nonzero, writes it into the outer link's members (an outer MIN or
  MAX that may close natively only where each instance is read once) or marks it inlined:
  its variable `DecideVarScope::INLINED`, with no column, its bounds still derived, no
  definition, and `SolverModel::Build` writing its members wherever a row or the objective
  reads it (`AddTermColumns`), its constants to a row's bound or to the objective's offset
  (`SolverModel::obj_offset`, which both backends add, so the objective a diagnosis reports
  is the one a kept helper gave). A MIN/MAX family of one member is defined by an equality,
  with no envelope, indicator or Big-M, wherever it is pinned on rigid rows
  (`EmitExtremumLink`). Speed with no model change: the
  backward bound pass skips a group with no helper term, a SUM definition is accumulated in
  one sparse row, and an extremum member's row scans while short (`MinMaxLinkRow`) -- which
  also took a third off Q4's composed MIN/MAX. The full benchmark found one regression of
  phase 6's own, an exact ABS's argument row built afresh per data row (Q10 at 300 ms of
  model construction against 232, Q3 at 19 against 13); one row reused for every data row
  brings them to 198 and 14.
- **Execution** (`../01_pipeline/08_execution/done.md`). Unchanged in what it computes --
  every helper's members and presence, the instances a helper's absence skips -- it hands
  `inlining` to stage 06 and runs the inlining just before the column space is built. It
  reports the ANR counters (`anr.frames`, `anr.frame_windows`, `anr.frame_positions`,
  `anr.helpers`, `anr.helper_instances`, `anr.members`, `anr.member_terms`,
  `anr.inlined_helpers`) under `DECIDB_PROFILE`.
- **Benchmarks.** `benchmark/decide/anr_scaling.py` (`../02_operations/benchmarking.md`) runs
  the nested, variant, point, range and cyclic workloads over synthetic tables from 10³ to
  10⁶ rows at fixed cardinalities and reports each one's growth: from 0.83 to 1.04 between
  10⁵ and 10⁶ rows for all but a MAX over windows of five (`range-max`), at 1.07 -- six
  million rows, each with its full provenance
  (`../05_performance/08_memory_and_scaling/todo.md`). It found that MAX's extremum links
  built for every window before any was emitted, 2.7 s at 10⁶ rows; built one window at a
  time, 1.3 s. The flat suite builds every model the pre-ANR baseline did, DeciDB's own time
  over it 1,675 ms against 2,105 (`../05_performance/04_model_construction/done.md`, "After
  ANR phase 7").
- **Choices beyond the plan's text.** The plan inlined a helper whose only consumer reads
  it; D83 inlines where no nonzero is added, which takes that case and the deck's own --
  West's sum of three terms written into the two rows reading it (p. 115) -- so F1 with an
  inner SUM is the deck's three rows over six shipments (p. 119). A helper is inlined as a
  whole, since its instances share one variable's columns: a MIN/MAX with a group of one
  among groups of several keeps a column for it, defined by its equality
  (`../01_pipeline/06_model_formulation/todo.md` §1). A clause folded into a helper's box
  keeps the helper (§2 there). `InlineSumOfGroupSums` stays where it is: it removes, at
  stage 05, a reducer whose square no helper could define (D75), before a helper exists,
  where this removes a helper's column after its members are listed -- not a second inliner
  of the same thing. EXPLAIN still names each helper, as inlining is decided on the data.
- **Model pins moved**, each worked out by hand in its test: F1_sum from twelve columns and
  eight rows to six and three, F1_max from twelve and ten to ten and eight
  (`test_anr_nested.py`); deck examples 5 and 8 lose their definition rows
  (`test_scope_deck_examples.py`, `test_scope_value_rewrite.py`, where two `by` keys keep
  only the region sums); deck example 3's per-product helpers are written into their bounds
  (`test_anr_composition.py`); the nested objective's sum over group extrema and its MIN
  over group sums lose their inner columns (`test_auxiliary_bounds.py`). The golden corpus
  changed in four models -- a MAX and a MIN over group sums, two `by` keys, the facility
  link, now the textbook row `x1 + x2 - 10 open <= 0` -- with two answers among ties.
- **Architecture review** (a `decidb-architect` pass over the phase-7 diff, no wrong answer
  found; every place a decision's column is asked for was checked). Adopted: an outer MIN
  or MAX that may close natively takes a helper only where each instance is read once,
  since a native extremum over expressions costs a column and a row per member (the
  reviewer's `max(sum(x + 1) by (t.product)) >= 15`, which had grown by a column and a row);
  the plan hands down where to inline (`inlining`, `inline_outer`), and stage 06 only counts,
  holding every read to the plan; every term of the builder goes through one function, the
  query-wide and inlined branches merged; an inlined helper's constants are the objective's
  offset, where they had been dropped, changing a diagnosis's achievable objective; reads of
  an outer SUM counted once per group, as its definition states them; a read of an absent
  instance, a fold whose outer link is not after it, and a row reading a folded helper are
  internal errors; the one-member rule moved into `EmitExtremumLink`, for every MIN/MAX
  site; the absorbed-bound test reads `LogicalDecide::HasAbsorbedBound`. Kept: `INLINED` is
  a scope value, as `FRAME_READ` is, so every `switch` over scopes must name it; the instance
  lookup stays where each stage reads it (execution, the inlining pass, `VarIndexer`).
- Tests: `test_anr_inlining.py` (`../04_testing/scope/done.md`) and
  `test/common/test_decidb_helper_inlining.cpp` (the pass on hand-built inputs: a sum
  written into two rows with its constant at the bound and the bound quoted as written, one
  written into the objective with its constants as the offset, a sum the plan does not allow
  and one whose readers would grow the model kept, a sum written into a maximum's members
  with its scale and constants, a closing maximum keeping a sum two members read, a plan
  inlining into a maximum pinned on both sides and a read the plan does not list as internal
  errors, a group of one member as an equality). The differential fuzzers against
  `_anr_oracle` (phase 6's compositions and phase 5's ranges, three solver settings) ran
  8,000 cases -- 4,000 on the build before the review, 4,000 on the final one -- with no wrong
  answer and no internal error; what they report is the known order of two errors (a plan-time
  "`abs` is never negative" before the oracle's data error).

## What guards the full algorithm

The risks its plan named, and what holds each now:

| Risk | Guard |
|---|---|
| A duplicate signature's row lost while it still feeds a deeper member or a timeline | Signature grouping decides *instances*; a reducer's members are listed from the rows its path keeps, never from the instances; F11 and the scope grid pin it |
| One `by` value under two ancestor groups sharing a helper instance | Helper keys are path keys (`syntax_reference.md` §5.5): F1's `West` reads rows of two depots and is one instance, a `(rid, cid)` pair one per pair |
| An under-constrained MIN/MAX helper | Both link sides past any consumer of unknown sign (D73); the `DECIDB_NATIVE_CONSTRUCTS` matrix |
| A stale Big-M in a repair model | Definitions are stated against the active `FormulationBox`, and inlining is decided at assembly, before the box: DIAGNOSE re-formulates the same model (`test_anr_diagnostics.py`, `test_anr_inlining.py`) |
| A frame's expansion blowing up | `decide_frame_limit` before allocation; the interrupt polled per timeline and per window |
| A variant silently undoing an inner `per`'s de-duplication (D65) | EXPLAIN ANALYZE's variant count; F11 pinned both ways; the trap in `syntax_reference.md` §5.3 |
| The frame words breaking SQL windows in a clause's subquery | The lexer's depth-aware `OVER_DECIDE` state; a window inside a clause's subquery is a test |
| A nested MIN/MAX refused on HiGHS where its bound is only at the root | The backward bound pass (`DeriveReducerMemberBounds`), which still flows through an inlined helper |
| A reducer shared by two clauses duplicating a nested helper tree | A known limit (`../01_pipeline/05_optimizer/todo.md` §1), pinned in a dump as the baseline for a sharing rule |
| A helper inlined where something still reads its column | The plan lists every reader (`MarkInlinableHelpers`), stage 06 holds every read to it, and `VarIndexer::Get` on an inlined helper is an internal error -- never a column that is not there |
| Two engines for one language | None: each phase landed on the one path, and the two-level objective's special case was deleted when nesting shipped |

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
| D36 | A removed objective form with no exact twin gets its own message: a flat `MIN`/`MAX`/`AVG` with `PER` is told that an objective is produced once (an outer `AVG`, pointed to `sum` while it had no twin, keeps its `avg` since D75) |
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
| D47 | The structural follow-ups — one clause-scope struct and walker (the two-level renderer went with the form, D75) — are a follow-up change (`../06_issues/code_quality/todo.md`) |
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
| D63 | D8 messages print the colon form as written (the determination message it also covered is retired, D64) |
| D64 | Variant signatures replace the determination errors: a scoped expression is one instance per class and distinct value of its tuple terms, its decisions' instances and its reducers' `by` keys, in constraints and reducer operands |
| D65 | An inner `per` and the colon form follow D64 too: a tuple whose body varies is one value per variant, which EXPLAIN ANALYZE counts |
| D66 | The objective is produced once: a second variant is an error saying it would be produced more than once |
| D67 | A frame's selected position holding rows that read different payloads is an error naming the position; rows reading one payload — one value of data, one linear form over decisions — select once |
| D68 | A frame's order key orders peers as `ORDER BY` does, NULL where `default_null_order` puts it |
| D69 | Under `per K`, each class lies in one timeline of every frame its body reads directly, else an error naming the class and the timelines |
| D70 | A cyclic range keeps every repeated visit: on three positions, `from 4 previous to 4 next` reads nine |
| D71 | A frame's `k` and `every` are positive whole-number constants; 0 is a parse error |
| D72 | A point frame is read in place, its decisions at the selected row's columns, inside a reducer operand too; a range over decisions is a helper decision per window, a timeline or a position on one |
| D73 | A nested MIN/MAX is pinned on the side its readers push it through SUM and AVG levels, and on both sides under any other reader |
| D74 | Stays refused, each by name: `norm` inside a reducer and a reducer inside `norm`, a square or product of decisions inside nested reducers (the objective's direct sums of group sums aside), a reducer over data alone (`count` included) added inside one over decisions, inside a reducer a reducer times a decision and `abs` over a reducer on a path that keeps rows; a frame in `when`/`per`/`by`/`within`/an order key, a reducer or frame inside a frame's payload, a square or product of decisions in it, a frame term times a decision |
| D75 | The two-level objective keeps its spelling and loses its special path: it is a nested reducer, its four refusals lifted |
| D76 | A frame is a signature field by its keys: its timeline, the anchor's position there when it reads relative to the anchor, and its fallback where reached; not by the value it reads |
| D77 | A signature's data field is a maximal decision- and reducer-free subtree, read on the canonical tree so the data terms of one additive context are one field; a decision is a field by instance |
| D78 | A NULL payload read at a selected row is a NULL coefficient or bound, the §3 error |
| D79 | Under the tightest-bound collapse an anchor whose frame selects nothing bounds nothing; a group with no such anchor present has no row |
| D80 | `else NULL` written, or a fallback that is NULL before the query runs, is no fallback |
| D81 | A reducer over decisions whose members a frame may drop is a helper decision: execution lists its members where the frame selects, and an instance left with none is absent -- its comparison skips that instance, a reducer around it drops that member, the objective is an error |
| D82 | Under `abs` or `norm` inside a reducer, a member a frame drops goes with the rows that would define it; a reducer that may be absent under `abs` inside another reducer is refused |
| D83 | A helper decision is written where it is read -- into rows, the linear objective, or another helper's members -- when that adds no nonzero to the model (an instance of `m` terms read `k` times: `m·k` against `m + 1 + k`), as a whole, and only where every reader can take it; a MIN or MAX group of one member is that member, an equality (deck p. 112) |

| # | Call |
|---|---|
| S1 | `per row` is accepted as the written-out default |
| S2 | Decisions are referenced by bare name; `D.open` is refused (D43) |
| S3 | With no `per`, a constraint is per row, so `sum(x) <= cap` takes the tightest `cap`; with an explicit `per`, each value of a varying bound is a variant (D64), which binds alike |
| S4 | `SUM(D: e)` reads the same classes and variants as `sum(per D: e)` |
| S6 | `per (a, b):` is refused with a message; parentheses mean only `per ()` |
| S7 | Inside a DECIDE clause `when`, `per`, and `by` after a `)` are keywords; a column with such a name is quoted there |
| S8 | `per` keys are columns and relations; expressions are allowed only in `by` |
| S9 | An input with no rows at all gives an empty result; D4 is about a `when` that removes every row of a non-empty input |
| S10 | Data-only reducers stay on the bound side of a comparison |
| S11 | A reducer over a lone query-wide decision (`SUM(cap)`) is refused |
