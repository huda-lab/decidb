# ANR language surface — implemented

Item 1 of the plan has shipped: **the new spellings parse**. Nothing binds them yet,
so the engine still speaks the language recorded in
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
- Every other scope, wherever it sits in the clause (subqueries included), answers
  *"DECIDE: 'PER key:' is not supported yet"* (or `PER ():`, `WHEN condition:`,
  `BY (key)`) from `ValidateDecideNoUnsupportedScope`, which each later item shrinks.
- The grammar's conflict budget fell from `%expect 6` to `%expect 4`: the two conflicts
  the postfix-operator states carried were on the token `PER` being readable as an
  identifier, which it no longer is inside the clause.
- Tests: `test/decidb/test_scope_parser.test` (accept, exact errors, boundaries, old
  spellings and plain SQL through the engine) and the scope case in
  `test/common/test_decidb_plan_serialization.cpp` (round trips including the deck
  examples 1–8 in both forms, a scope covering exactly one constraint, `PER ROW`
  equivalence, parse errors at the parser).
