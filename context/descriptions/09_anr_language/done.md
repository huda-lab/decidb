# ANR language surface — implemented

Items 1 and 2 of the plan have shipped: **the new spellings parse**, and **a
declaration's `per` runs**. What the engine accepts is recorded in
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
- `PER ROW` is the default written out: it leaves no node and runs as if omitted.
- Every scope in a constraint or the objective, subqueries included, answers
  *"DECIDE: 'PER key:' is not supported yet"* (or `PER ():`, `WHEN condition:`,
  `BY (key)`) from `ValidateDecideNoUnsupportedScope`, which each later item shrinks; a
  declaration's `PER` binds since item 2.
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
- Until item 3, a keyed decision inside `SUM(D: e)` is refused with "not supported yet"
  (D10). Until item 5, a keyed decision beside a reducer in an aggregate constraint is
  refused, named as declared, with a `per ()` hint (D11). DIAGNOSE labels its scope
  `entity` until item 6 (D11).
- `exact_key` (id 105) and `declared_key` (id 102) are serialized; `nodes.json` names
  `duckdb/planner/operator/decide/logical_decide.hpp`, so regenerating reproduces the
  committed code.
- Tests: `test/decide/tests/test_scope_declare.py` with the reference evaluator
  `_scope_oracle.py` (classes in Python, model through the oracle solver, DeciDB's answer
  judged class by class); the keyed-scope case in
  `test/common/test_decidb_plan_serialization.cpp` (sharing, `exact_key`, round trip);
  the declaration cases of `test/decidb/test_scope_parser.test`, which now run.
