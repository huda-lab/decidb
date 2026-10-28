# How scopes are built

How `per`, `when` and `by` (see [guide.md §1.1, §5–§7](guide.md#11-scopes-per-when-by)) are carried from the parser
to the solver, and why each layer does what it does. Stage details are in `../README.md#pipeline-stages`.

## The design in one paragraph

A scope is data attached to a clause, not a rewrite of it. The binder wraps a clause in a `when` and a `per` marker and
tags each reducer with its own filter, key and `by` key; the canonicalizer reads those markers once to choose the
clause's shape and to build the data checks; execution partitions rows by each key and reads only the rows a `when`
keeps. No layer turns a scoped clause into a different unscoped one, so EXPLAIN, DIAGNOSE and messages can always quote
the clause the user wrote.

## Parser

- The lexer filter `base_yylex` (`third_party/libpg_query/src_backend_parser_parser.cpp`) turns the words into
  DECIDE-only tokens inside a DECIDE clause: `WHEN_DECIDE` (`WHEN_DECIDE_OBJECTIVE` after `MAXIMIZE`/`MINIMIZE`),
  `PER_DECIDE`, `PER_DECIDE_PAREN` before `(`, `PER_DECIDE_ROW` before `ROW`, and `BY_DECIDE` for a `BY` right after
  `)`. Ordinary SQL never sees these tokens, so `GROUP BY`, `CASE WHEN` and columns named `per` are unaffected. A
  `WHEN` inside `CASE … END` stays SQL's (`decide_case_depth`) so the binder can refuse the `CASE` by name.
- The lexer state is pushed at every `DECIDE` and `SUCH` and popped when the clause reduces
  (`PGDecidePushLexState` / `PGDecidePopLexState`), so a nested DECIDE leaves the outer clause's keywords intact.
- Grammar (`third_party/libpg_query/grammar/statements/select.y`): `decide_when`, `decide_per`, `decide_scope`,
  `decide_by`. `applyDecideScope` builds the markers `SCOPE_WHEN_TAG`, `SCOPE_PER_TAG` (children: body, then key) and
  `SCOPE_PER_ROW_TAG`; `by` is `REDUCER_BY_TAG`. `%expect 4`, all from the optional declaration slot.
- The removed spellings keep error-only productions; `RefuseRemovedDecideSpellings`
  (`src/parser/decide/decide_removed_spellings.cpp`) throws the message, built from the user's own expressions. It
  runs from `TransformSelectNode`, and again at the top of `BindDeclarations` for a parse tree stored in a view or
  macro (D54). The predicates over parsed markers that the refusal and the binder share are in
  `src/parser/decide/decide_parsed_markers.cpp`.

## Binder

- `ValidateDecideScopeSpellings` (`decide_binder.cpp`) checks where a scope and a `by` may stand and that conditions
  and keys use data only. `RefuseQualifiedDecisionReferences` (`decide_declarations_binder.cpp`) refuses `D.x`.
- **Declarations.** `BindDeclarations` resolves a key through `FindOrCreateKeyScope`: each element against the query's
  own `FROM` (a declared decision is refused first, then a column, then a relation standing for its stored columns).
  The key is sorted and de-duplicated by binding (`SortedKey`), so equal keys share one `EntityScopeInfo`
  (`FindOrCreateExactScope`). `DecideVarScope` records `ROW`, `ENTITY` (keyed) or `SCALAR` (query-wide); a keyed
  declaration's `DecideVarScopeInfo` also keeps the key as written (`declared_key`) for messages. It is one struct
  rather than parallel arrays so an auxiliary appended by the optimizer cannot desync a scope from its key.
- **Constraints.** `DecideConstraintsBinder::BindScopedConstraint` binds the body, then wraps it: `when` as a
  conjunction tagged `WHEN_CONSTRAINT_TAG` (child 0 the constraint, child 1 the condition), `per` as one tagged
  `PER_CLASS_TAG` (child 0 the constraint, children 1.. the key columns, none for `per ()`). The key's written text
  is recorded as a source fragment.
- **Reducers.** `DecideBinder::BindScopedReducer` puts the inner `when` on `BoundAggregateExpression::filter` and tags
  an inner `per` with its key scope (`MakeQualifiedReducerTag`). `BindReducerBy` registers the `by` key in
  `LogicalDecide::group_keys` (one entry per key as written; keys compare as sets, a subquery by its text,
  `FindOrCreateGroupKey`) and tags the reducer `MakeGroupKeyTag`. `BindQualifiedReducer` binds the colon form onto the
  same key scope as `sum(per D: e)` and adds `COLON_FORM_TAG`, which only affects how it prints. Tags are composable
  (`AddDecideTag`, `HasDecideTag`), because a keyed `AVG` is tagged by the binder and again by the AVG→SUM rewrite.
- **Objective.** `DecideObjectiveBinder::BindScopedObjective` wraps a front `when` the same way; `per ()` adds
  nothing. `BindTwoLevelObjective` binds `outer(per k: inner(e) by (k))` as `outer(inner(e))` under a
  `PER_CONSTRAINT_TAG` wrapper whose children are the key columns.
- `CASE WHEN` is refused with one message wherever it sits (`DecideCaseUnsupportedMessage`).

## Canonicalizer — shape and checks

`DecideCanonicalizer::ClassifyScoped` (`decide_canonicalizer.cpp`) reads a comparison's decision side once and picks
its shape ([guide.md §6.4](guide.md#64-how-a-clause-becomes-model-rows)):

| Shape | When | Model rows |
|---|---|---|
| ROWS | no decision-bearing reducer | one per class, from its first kept row |
| AGGREGATE | decision reducers sharing one `by` key, whole-query factors, only query-wide terms beside | one per `by` group, under the group's tightest bound |
| VALUE | anything else (per-row or keyed decision beside a reducer, row-varying factor, different `by` keys, a reducer inside an expression, a product of decisions beside a reducer, a `MIN`/`MAX` beside another term with a `by`, a non-constant bound or a query-wide decision) | each decision reducer becomes a helper decision, one instance per `by` group; then one row per class |
| UNSUPPORTED | `AVG` under `<>` with anything but plain `AVG`s | refused |

The shape is decided here, once, because a later layer that re-decided it would need its own copy of these rules; the
canonicalizer never opens a term, so a filtered or grouped reducer is one atom to it.

- **VALUE.** The comparison is stamped `VALUE_REWRITE_TAG`. `DecideOptimizer::RewriteScopedReducers`
  (`decide_rewrite_scoped.cpp`) replaces each decision reducer with a helper decision (`__value_<n>__`, reused for a
  repeated occurrence in the same clause) and records it in `LogicalDecide::reducer_value_links`. The helper is
  `INTEGER` when the reducer's value is whole (`DecideValueIsWholeNumbered`, the same test the binder's strict-bound
  gate uses), query-wide for `by ()`, otherwise keyed on its own `by` groups among the clause's kept rows. A
  `MIN`/`MAX` helper gets the envelope side its rows push it toward, the closing side otherwise, both for `=`, `<>` or
  an unknown sign (`ExtremumPushOf`). Stage 06 (`linearization_reducer_value.cpp`) boxes each helper by its members'
  reach (`DeriveReducerValueBounds`, alternated with the ABS bound derivation to a fixed point) and emits its
  definition: an equality row per group for `SUM`/`AVG`, an extremum link per group for `MIN`/`MAX` through
  `EmitExtremumLink`.
- **Factors.** `PeelScale` moves a factor outward onto the reducer and keeps it there to the solver row, so `MIN`/`MAX`
  stay exact under a negative factor. It refuses a decision as the factor and a row-varying factor in the objective.
  Whether a column is query-wide is not a shape question — after flattening, a column, an uncorrelated and a
  correlated subquery all look alike — so `plan_select_node.cpp` hands the canonicalizer an allow-list of the table
  indexes uncorrelated subqueries flattened into. Anything not on it is row-varying. (The reverse test, "not the
  reduced relation's own column", would silently admit a correlated subquery.)
- **Easy `MIN`/`MAX` absorption.** An easy `max(m) <= K` / `min(m) >= K` with no `when` and no filter, whose member is
  affine in one decision with constant coefficients, is a bound on every member, absorbed into the decision's box
  (`DecideReadSingletonRow`, `decide_singleton_row_folding.cpp`, D41), in any affine spelling with a constant other
  side: `max(-x) <= 2` is `x >= -2`. The reducer stays in the tree so EXPLAIN and DIAGNOSE quote the clause as written.
- **Scope checks.** `BuildConstraintScopeChecks`, `BuildClauseScopeChecks`, `BuildReducerScopeChecks` and
  `BuildObjectiveScopeChecks` build `DecideScopeCheck`s of three kinds — `KEEPS_A_ROW` (D4), `DETERMINES`,
  `READS_A_ROW` (D8) — on `LogicalDecide::scope_checks`, from the clauses as bound. The bound-side window rewrite adds
  a fourth, `VALUE_IS_VALID`, for the data values bounds read. `PhysicalDecide::RunScopeChecks` runs them all before
  any clause is evaluated. They are run on the data rather than proved statically because whether a key determines a
  column is a property of the rows, and a static rule would refuse queries that are fine on their data.

## Optimizer

- `RewriteScopedReducers` (VALUE shape, above), after `RewriteBilinear` and before the MIN/MAX passes.
- `RewriteMinMaxObjective` (`decide_rewrite_minmax.cpp`) reads the two-level wrapper into `per_inner_*` /
  `per_outer_*` (reducer kinds, easy or hard); execution groups by `DecideObjective::per_columns`. An inner
  `MIN`/`MAX` emits linking rows only for rows with a nonzero coefficient; a group with none has its auxiliary pinned
  to 0.
- `RewriteRhsAggregates` (`decide_rewrite_rhs_window.cpp`) turns every reducer on a bound into a window column over
  the source, partitioned by its `by` key over the rows the clause keeps, and reduces a varying bound to its group's
  tightest value (keeping every value for `<>`).
- Rows a rewrite adds for a clause (`IN`, `ABS`, `norm` L0) keep the clause's `when`.

## Execution

In `src/execution/operator/decide/physical_decide.cpp`:

- `RunScopeChecks` first. Then each clause reads only its kept rows: `KeptRows` / `KeepRowsWhere` evaluate a condition
  (the `and`/`or` order of guide §6.3), and `ExecuteOnReadRows` runs every other expression only on the rows a mask
  marks, NULL elsewhere.
- `BuildGroupIds` partitions rows by a key, NULL a class; `LookupOrBuildPerGroupIds` caches one partition per key so
  every clause and check on the same key shares the numbering. An empty or constant key is one group.
  `CollectGroupClasses` lists the classes a model row stands for (DIAGNOSE's `group` column).
- `BuildEntityMappings` gives every row its class's solver column, for keyed decisions and for helpers.
- `BuildQualifierKeepMask` keeps one row per class of an inner `per` (or colon form) in each group, among the rows
  the reducer reads; `ApplyQualifierToFilter` ANDs it into the term's filter. AVG's denominator then counts classes
  without special code. Which row survives cannot matter, because `DETERMINES` has checked every row of a class
  carries the same value. The mask is per term, so `SUM(D: a * open) + sum(b * ship)` de-duplicates only the first.
- The prepared model (`decide_prepared_model.hpp`) carries the scope on `DecideConstraint` — `when_condition`,
  `group_key`, `per_key` (`PerKeyRole`: `NONE`, `ONE_ROW_PER_CLASS`, `NAMES_CLASSES`) and `class_key` — and on
  `DecideObjective` — `when_condition` and `per_columns`.

## Rendering

`CollectDecideExpressionStrings` (`decide_source_provenance.cpp`) walks the wrappers, building the front qualifier
from `WhenQualifier` and `PerClassQualifier` (`FrontQualifier`, `AttachDecideQualifier`). `RenderTwoLevelObjective`
prints the two-level form; `RenderReducer` prints a reducer's own scope and `by`. See [explain.md](explain.md).

## Scale

A clause emits one row per class (ROWS, VALUE) or per `by` group (AGGREGATE), so a high-cardinality key multiplies
the model. The optimizer's work on large models is in `../pipeline/05_optimizer/done.md`.

## Tests

The scope suites, the `_scope_oracle.py` reference model and `_brute_force.py` are described in
`../testing/scope_suite.md`: `test/decide/tests/test_scope_*.py` and `test/decidb/test_scope_parser.test`.
`test_scope_combinations.py` runs 784 cells against the oracle. `test_no_old_spellings_remain.py` keeps removed
spellings out of every test but `test_scope_removed_syntax.py`.
