# Stage 01 — Parser

Turns SQL text into a parsed `SelectNode` carrying `decide_variables`,
`decide_constraints`, `decide_sense` and `decide_objective`, refuses the removed DECIDE
spellings with a message that names their replacement, and applies the
source-preserving validation that must see what the user actually typed.

**This stage decides no shape.** Which side of a comparison a term sits on, which
way the relation points, and where a factor on a reducer lives are all decided
once by `DecideCanonicalizer` on the bound tree — see
[`../04_canonicalizer/done.md`](../04_canonicalizer/done.md).

---

## 1. Grammar

`third_party/libpg_query/grammar/statements/select.y`. `DECIDE`, `MAXIMIZE` and
`MINIMIZE` are reserved keywords (`third_party/libpg_query/grammar/keywords/reserved_keywords.list`), as
is `SUCH`.

### Two clause orders

Both build the identical `PGDecideClause` through `makeDecideClause()`:

| Order | Shape | Nonterminals |
|---|---|---|
| Single block | `SELECT ... FROM ... WHERE ... DECIDE x(INT) SUCH THAT ... MAXIMIZE ...` | `decide_clause` → `DECIDE typed_decide_variable_list opt_decide_tail` |
| Split (paper's) | `SELECT ... DECIDE x(INT) ... FROM ... SUCH THAT ... MAXIMIZE ...` | `decide_declaration` (after `into_clause`) + `decide_body` |

`decide_declared_before_from` records that the declaration slot fired, so a
second `DECIDE` arriving through `decide_body` reports *"DECIDE appears twice;
declare the variables either before FROM or with SUCH THAT, not both"* instead of
a generic syntax error.

### Variable declarations

`typed_decide_variable` accepts a name with its type, alone or behind a `per` scope;
the type is mandatory:

```
x(TYPE)             one decision per result row
per row: x(TYPE)    the same, the default written out
per K: x(TYPE)      keyed — one decision per class of the key K
per (): x(TYPE)     query-wide — one decision for the whole query
```

Each builds a `PG_AEXPR_OF` node pairing a one-field `PGColumnRef` with the type; with
a `per`, `applyDecideScope` wraps the name in its scope marker. The other alternatives
exist **only to produce an actionable message** rather than a parse failure: a name with
no type (`x`, or `per K: x`: *"DECIDE variable "x" needs a type; write x(INT), x(BOOL) or
x(REAL)"*, with the scope repeated by `decideScopeText`), the retired `x IS INTEGER`
form, and the declarations syntax_reference.md §10.3 lists as removed, whose messages
name the `per` spelling that replaces them (`decideTypeWord` turns the type marker back
into `INT`, `BOOL` or `REAL`). `scalar` is an unreserved keyword, so it still names a
column or a decision (`DECIDE scalar(INT)`).

`decide_constraint_list` joins its items with `AND`. A comma is refused with
*"comma-separated SUCH THAT constraints are not supported; use AND between
constraints"*, unless `absorbRemovedPerKey` folds it into the key of a removed `PER`
written after a constraint with a comma, so that message names the whole key.

### Scopes: `when`, `per`, `by`

A scope is written *in front* of what it governs (syntax_reference.md §1.1):

```
decide_scope  ::= decide_when | decide_when decide_per | decide_per
decide_when   ::= WHEN_DECIDE a_expr | WHEN_DECIDE_OBJECTIVE a_expr
decide_per    ::= PER_DECIDE expr_list | PER_DECIDE_PAREN '(' ')' | PER_DECIDE_ROW ROW
decide_by     ::= BY_DECIDE '(' ')' | BY_DECIDE '(' expr_list ')'
```

- `decide_constraint_item` and `decide_objective_item` are each
  `decide_scope ':' decide_item_expr` or the bare `decide_item_expr`. A
  `decide_scope ':'` may also precede the first argument of a function call
  (`func_name '(' decide_scope ':' func_arg_list ')'`), and `decide_per ':'` a
  declared name. `decide_by` may follow a `func_application`, a scoped call, or the
  colon form.
- The scope is carried through the grammar as a `PGDecideScope` (`parsenodes.hpp`)
  and folded by `applyDecideScope` (`grammar.cpp`) into `PG_AEXPR_SCOPE_WHEN`
  (`lexpr` body, `rexpr` condition) inside `PG_AEXPR_SCOPE_PER` (`lexpr` body,
  `rexpr` the key list, `NULL` for `per ()`). `per row`, the default written out,
  builds `PG_AEXPR_SCOPE_PER_ROW` (`lexpr` body, no key), so a clause keeps the
  spelling it was written in (D19). `by` builds `PG_AEXPR_REDUCER_BY` (`lexpr` the
  call, `rexpr` the key list, `NULL` for `by ()`).
- A key element must be a column or relation name (`makeDecideScope`): anything
  else is *"a per key lists columns or relations; put expressions in by (...)"*.
  A parenthesized key is *"write per a, b: without parentheses; parentheses are
  only for per ()"*. A `per` other than `()` on an objective is *"an objective is
  produced once: write per (): or leave per out"*.
- Because the colon ends the scope, the condition is a full `a_expr` and needs no
  parentheses. A `WHEN` or `PER` written after a clause that already has a scope in
  front is a plain syntax error.
- `decide_item_expr` closes a constraint or objective body at `DECIDE_ITEM`
  precedence. That precedence is above `AND`, so a top-level `AND` separates
  constraints and `per g: A AND B` scopes only `A`, but below the comparison
  operators and the DECIDE `WHEN` tokens, so the body keeps its SQL expression shape.
  No parsed-tree association repair is needed.

### The colon form

`SUM(D: e)` and `SUM(D, T: e)` are `func_name '(' func_arg_list ':' func_arg_list ')'`,
built by `makeQualifiedReducer` into `PG_AEXPR_QUALIFIED_REDUCER`. The qualifier is
read as a `func_arg_list` because `sum(D, T: e)` and `sum(a, b)` share a prefix
LALR(1) cannot tell apart at the comma; the decision moves to `:` versus `)` after a
completed list, which is conflict-free. Every element must be a bare relation name, and
the whole list travels as one `PGList`; the binder resolves what it means
(`../02_binder/done.md`, "Relation-qualified reducers").

### Productions kept for the removed spellings

The forms syntax_reference.md §10.3 lists as removed — a `WHEN` or `PER` written after
a clause or a reducer — still parse, so that the transformer can refuse each with a
message built from the user's own expressions (§3):
five alternatives of `decide_constraint_item` and of `decide_objective_item` (a `WHEN`,
a `PER` with one or several columns, or both, after the item), and the `c_expr`
alternatives of a `WHEN` after a call or after the colon form. They build
`PG_AEXPR_WHEN_CONSTRAINT` and `PG_AEXPR_PER_CONSTRAINT` markers, nested
`PER(WHEN(body, condition), key)` when both are written.

- After a constraint the condition is an `a_expr`, so `NOT f` or `g IN (...)` is named
  as written; `WHEN_DECIDE` outranks `AND`, so it ends where the next constraint
  begins.
- After the objective, `WHEN_DECIDE_OBJECTIVE` takes `decide_objective_when_condition`:
  a `c_expr` or one comparison between atomic operands, since an `a_expr` there would add
  three conflicts. A longer condition (`IN`, `IS NULL`, `BETWEEN`, `LIKE`) parses as
  operators around the marker, and the transformer reads the whole condition back
  (`WrittenCondition`).
- After a call in a constraint, the condition is a `c_expr`
  (`decide_aggregate_when_condition`), so a comparison there needs parentheses to reach
  the message; without them it is a syntax error. In the objective it takes the
  objective's condition rule above.

### Conflicts

`grammar.y` declares `%expect 4`, itemised in its header comment: all four come
from the optional declaration slot, one per `simple_select` alternative. They are
reachable only when `from_clause` and `where_clause` are both empty, and both
derivations build the same node, so bison's default shift is not load-bearing.
The `WHEN_DECIDE` conflicts — the empty `within_group_clause` and the plain colon
form — are resolved explicitly by `DECIDE_ITEM` precedence, below both DECIDE `WHEN`
tokens, so the parser shifts `WHEN`; they are keyed on tokens `base_yylex` emits only
inside DECIDE. `PER` and a reducer's `BY` reach the grammar as DECIDE-only tokens, so
neither can be read as an identifier there.

Grammar changes require `make grammar-build` (bison 2.3).

---

### The `DIAGNOSE` prefix

`third_party/libpg_query/grammar/statements/variable_show.y`. `DIAGNOSE <SelectStmt>` is
one more alternative on `VariableShowStmt`, mirroring `SUMMARIZE`: it wraps a `SelectStmt`
and sets `is_diagnose` on `PGVariableShowSelectStmt`. `DIAGNOSE` is a reserved keyword.

Mirroring `SUMMARIZE` rather than `EXPLAIN` buys two things. `EXPLAIN` carries its own
statement node and an options syntax (`EXPLAIN (VERBOSE) …`) the prefix does not need; and
`select_with_parens` already has a `'(' VariableShowStmt ')'` production, so
`SELECT * FROM (DIAGNOSE …)` composes with no further grammar work.

The prefix is parse-only here — the parser does not check that the inner query has a
`DECIDE` clause. That is a binding question (the clause may sit inside a subquery), and
`Binder::BindDiagnose` answers it.

## 2. Lexer gating: the DECIDE keyword tokens

`third_party/libpg_query/src_backend_parser_parser.cpp`, in `base_yylex`.

DECIDE's `WHEN` cannot be the global SQL `WHEN`: after a function call it
collides with `WITHIN GROUP` and corrupts ordinary function-call parsing. So
the lexer emits DECIDE-only tokens inside the clause:

- `DECIDE` **or** `SUCH` sets `in_decide_clause` and resets `decide_case_depth`,
  after saving the enclosing clause's state (`PGDecidePushLexState`,
  `third_party/libpg_query/include/parser/gramparse.hpp`). `SUCH` re-arms the flag
  for the split clause order, where the declaration slot popped it so
  `FROM` / `JOIN ... ON` / `WHERE` lex as ordinary SQL.
- The `decide_clause`, `decide_declaration` and `decide_tail` grammar actions pop
  that state once their clause is closed — one pop per `DECIDE`, one per `SUCH`.
  With nothing stacked a pop leaves the clause disarmed, which is what a
  non-nested query sees.
- **Nesting.** A DECIDE query may be a subquery inside another DECIDE clause. The
  saved state is what makes the inner clause restore its parent instead of
  disarming it, so a scope written after the subquery still lexes as DECIDE's.
  `decide_declared_before_from` is saved with the rest, so an inner declaration
  cannot make the outer clause look like a duplicate `DECIDE`. Sixteen levels are
  restored exactly; deeper still parses. Pinned by
  `test/decide/tests/test_nested_decide.py`.
- While armed, `CASE` increments `decide_case_depth` and `END` decrements it, so a
  `CASE ... WHEN ... END` keeps its ordinary `WHEN` and still parses.
- A depth-0 `WHEN` becomes `WHEN_DECIDE`. After `MAXIMIZE` or `MINIMIZE`,
  `in_decide_objective` makes it `WHEN_DECIDE_OBJECTIVE`. The scope grammar reads the
  two alike (`decide_when`); the objective's own token lets the production kept for a
  removed `WHEN` after the objective read a comparison as its condition (§1).
- `BY` directly after `)` becomes `BY_DECIDE` (`decide_after_rparen` remembers
  the previous token). `GROUP BY`, `ORDER BY` and `PARTITION BY` always follow a
  keyword, so those clauses stay plain in a subquery written inside the clause; a
  `by` that does follow `)` there (a table alias in `FROM (SELECT 1) by`, say) is
  still `BY_DECIDE`.
- `PER` is split by the token after it, through the same one-token lookahead
  `NOT`, `NULLS` and `WITH` use: `PER_DECIDE_PAREN` before `(`, `PER_DECIDE_ROW`
  before `ROW`, otherwise `PER_DECIDE`. `per ()`, `per row` and `per <key>` are
  not LALR(1)-separable without it, since `(` also opens an expression and `ROW`
  is also a column name.

Inside a DECIDE clause, subqueries written there included, these words are
therefore keywords in those positions, never names: a column called `per`, a `by`
after `)` or a `row` after `per` is written quoted there (`"per"`), as the printer
does. Outside the clause nothing changes.

---

## 3. Transformer

`src/parser/transform/statement/transform_select_node.cpp:104-121` moves the
clause onto the `SelectNode`: `decide_variables` (via `TransformExpressionList`),
`decide_constraints`, `decide_sense` (`MAXIMIZE` / `MINIMIZE` / `FEASIBILITY`
when no objective was written), and `decide_objective`. It then calls
`RefuseRemovedDecideSpellings` on the node.

`src/parser/transform/statement/transform_show_select.cpp` turns the `DIAGNOSE` prefix
into a `ShowRef` with `ShowType::DIAGNOSE` wrapping the inner query node — the same
mechanism `SUMMARIZE` and `DESCRIBE` use, so the prefix reaches the binder as a table
reference rather than as a statement flag it would have to thread separately.

`src/parser/transform/expression/transform_operator.cpp` converts every DECIDE
`PG_AEXPR` node kind the same way — `lexpr` becomes child 0, `rexpr` (absent, one
node, or a list) the children after it — into a tagged `FunctionExpression` with
`is_operator = true` (the tags are in `common/enums/decide.hpp`):

| Parsed node | Becomes |
|---|---|
| `PG_AEXPR_SCOPE_WHEN` | `FunctionExpression(SCOPE_WHEN_TAG, [body, condition])` |
| `PG_AEXPR_SCOPE_PER` | `FunctionExpression(SCOPE_PER_TAG, [body, key...])` |
| `PG_AEXPR_SCOPE_PER_ROW` | `FunctionExpression(SCOPE_PER_ROW_TAG, [body])` |
| `PG_AEXPR_REDUCER_BY` | `FunctionExpression(REDUCER_BY_TAG, [reducer, key...])` |
| `PG_AEXPR_QUALIFIED_REDUCER` | `FunctionExpression(QUALIFIED_REDUCER_TAG, [aggregate, relation...])` |
| `PG_AEXPR_WHEN_CONSTRAINT` | `FunctionExpression(WHEN_CONSTRAINT_TAG, [body, condition])`, a removed spelling |
| `PG_AEXPR_PER_CONSTRAINT` | `FunctionExpression(PER_CONSTRAINT_TAG, [body, col...])`, a removed spelling |

The first five reach the binder, which binds every one into the bound tree's own
wrappers and tags (`../02_binder/done.md` §4a). The last two never leave this stage.

### Refusing the removed spellings

`RefuseRemovedDecideSpellings` (`src/parser/decide/decide_removed_spellings.cpp`) walks
the declarations, the constraints and the objective and throws a `ParserException` at
the first removed form, outermost first: a form written after a whole constraint is named
before one inside it. `TransformSelectNode` calls it on every select node, after the
node's `WHERE`, `GROUP BY`, `HAVING` and `QUALIFY` are built: a subquery inside a clause
is read with the clause's spellings, so its own expressions are checked as well
(`RefuseInExpressions`). The walk itself does not enter a subquery body; each node is
checked when it is transformed. Every message starts `DECIDE: `, carries the phrase
"is removed", and names the new spelling built from the user's own expressions
(`Written` drops the one pair of parentheses `ToString` puts around a comparison and
prints `count_star()` as `count(*)`).

The old grammar read a reducer's `WHEN` condition as one term (a column, a constant, a
call, or a parenthesized expression), and the objective's also as one bare comparison of
two terms; the rest of a longer condition parsed as operators around the marker
(`SUM(cap) WHEN cap BETWEEN 2 AND 5` is `BETWEEN(SUM(cap) WHEN cap, 2, 5)`).
`WrittenCondition` rebuilds the condition as written: it follows every operator written
after the operand the condition ended with, up to the outermost one that yields a
condition, and joins their text in order. A condition that was not one term was written
in parentheses and is whole, so the operators after it belong to the clause; an operator
whose other operands hold a reducer belongs to the clause too. A `WHEN` after something
that is not a constraint whose condition read on into a comparison
(`sum(x) by (g) WHEN (c) <= 5`) keeps the comparison's left side (`SwallowedCondition`).
`ReadReducer` reads a reducer once -- removed `WHEN`s around it, its `by`, the colon form,
the scopes in front of its argument -- and `ReducerSpelling` spells it once, joining a
removed condition to the reducer's own `when`:

- `RefusePostfixWhen` and `RefusePostfixPer`, after a constraint: the scope to write in
  front, plus "by (g) after each of its reducers" when the constraint holds a reducer,
  the NULL note (`NullNote`: a NULL key value is a class of `per`, so the message shows
  the `when g is not null` that leaves those rows out), and a note for a `WHEN` after one
  of its reducers (`ReducerWhenNote`);
- `RefuseReducerWhens`, after a reducer: every such `WHEN` of the clause, each as the
  reducer's own `when` -- `sum(when c: e)`, `sum(when c per D: e)` for the colon form,
  `sum(when d and c: e) by (g)` after a reducer with a scope or a `by`; `count(*)`
  becomes `sum(when c: 1)`, and `norm` (which takes no `when` inside it) is pointed at the
  scope in front; a constraint with no reducer is told to drop a postfix `PER`;
- `RefuseObjective` and `RefuseObjectivePer`, after the objective: the `when` in
  front, the nested form `outer(per g: inner(e) by (g))` for a reducer over a reducer
  (a `WHEN` inside the inner reducer goes in front; an inner colon form is
  `inner(per D: e)`; under a factor or beside a term, the nested reducer is written so
  where it stands, `syntax_reference.md` §5.5), with the NULL note the old form's dropped NULL keys call for, and *"an
  objective is produced once"* for a flat reducer (a flat `avg` is pointed at the sum of
  one average per class, NULL keys left out, which is what it meant);
- `RefuseDeclaration`, for a declaration in a parse tree stored before the removal (a
  view or a macro): a qualified name or a `scalar_` type marker. A fresh query never
  reaches it; the grammar refuses both (§1).

The binder calls the same function at the top of `BindDeclarations`, for a parse tree it
receives already built (D54). A decision referenced through a relation (`D.x`) has no
grammar signal; the binder refuses it (`RefuseQualifiedDecisionReferences`, §4). The
exact messages are pinned by `test/decide/tests/test_scope_removed_syntax.py`
(`../../04_testing/scope/done.md`), and the CLI test runner (`test/decide/decidb_cli.py`)
fails any query whose stderr says "is removed" unless the call says it is about one
(`removed_spelling=True`).

---

## 4. Parsed-tree validation

These run on the parsed tree before any DECIDE expression binds, at the top of
`DecideDeclarationsBinder::BindDeclarations`
(`src/planner/expression_binder/decide/decide_declarations_binder.cpp`), the first
consumer of the clause. The DECIDE section of `BindSelectNode`
(`bind_select_node.cpp`) only calls it.

In execution order:

| # | Operation | What it does |
|---|---|---|
| 1 | `RefuseRemovedDecideSpellings` | §3, for a parse tree stored before the removal. |
| 2 | `RefuseQualifiedDecisionReferences` | Refuses `D.x` where `D` is a relation of the FROM clause with no column `x` and `x` is a decision, in the constraints, the objective, the select list and `ORDER BY`: *"DECIDE: write x, not D.x; a decision is referenced by its bare name"*; in a `when` condition, a `per` key or a `by` key, *"DECIDE: D.x is a decision; when, per and by may only use data"* (D43). A qualified name that binds to a column, a struct field or a `USING` column is left to the SQL binder. |
| 3 | `ValidateDecideScopeSpellings` | Where `when`, `per` and `by` may stand (`../02_binder/done.md` §4a). |
| 4 | `ValidateDecideNoExplicitDecisionCasts` | Rejects `CAST`, `TRY_CAST` and `::` whose child subtree contains a decision reference, in `SUCH THAT` and the objective. Target type is irrelevant. Must run here: a source cast and a binder cast are both `BoundCastExpression`, so authorship cannot be recovered later. The post-solve `SELECT` projection is not scanned. |
| 5 | `ValidateDecideNoNonLinearScalar` | Rejects a scalar function over a decision other than `ABS` and `POWER(..., 2)`, a decision in a divisor, and a data-only reducer scaling a decision (`x * avg(cap)`, `sum(x) / sum(cap)`), before anything downstream has to interpret them. |
| 6 | `ValidateDecideNoIntegerStepComparisonOnReal` | Rejects `<`, `>` and `<>` over a `REAL` decision, in the constraints (`../02_binder/done.md` §4). |
| 7 | `TagDecideSourceFragments` | Stamps each cast and scalar subquery of the constraints and the objective with an id, so diagnostics can quote the user's own SQL after the tree has been rebuilt. |

`norm()` and DECIDE-variable `IN (...)` are deliberately not rewritten here.
The binder retains bound markers, and stage 05 chooses the indicator and linking
formulation after types, scopes, and DuckDB coercions are known.

---

## 5. Statement rendering and parsed serialization

The parsed tree stores DECIDE syntax as private structure: a variable declaration is
a comparison against a type-marker string, and a scope, a `by` and the colon form are
tagged `FunctionExpression`s. Those encodings are for the parser and binder, not SQL
users.

`SelectNode::ToString()` reverses the encoding. It emits `x(INT)`, `PER key: x(INT)`,
`PER (): x(REAL)` and `PER ROW: x(BOOL)`, renders the top-level constraint list without
generic SQL parentheses that the DECIDE grammar cannot accept, and restores objective
direction. `FunctionExpression::ToString()` restores the scope `WHEN condition PER key:
body` (WHEN first, as written, although PER is the outer tag), `PER ROW: body`,
`reducer BY (key)` and `SUM(D, T: expression)` wherever those tags occur inside a larger
expression. A marker of a removed spelling, and a `scalar_` type marker, print as
written, so a view stored before the removal still shows its own SQL and is refused when
bound. Ordinary parsed functions keep DuckDB's generic renderer.
`SelectNode::decide_sense` defaults to `FEASIBILITY`, so generated parsed-statement
serialization also has a defined value for ordinary non-DECIDE selects.

This is a structural round trip, not source-text preservation: DuckDB may normalize
case, quotes, parentheses, or the accepted split clause order. The contract is:

```text
SQL -> parsed SelectStatement -> ToString SQL -> equal parsed SelectStatement
```

Parsed-statement binary serialization is exercised separately from logical-plan
serialization. The regression matrix in
`test/common/test_decidb_plan_serialization.cpp` covers every variable type and
declaration scope, both clause orders, a constraint's `when` with a multi-column `per`
and a `by`, a reducer's own `when` and `per`, the one- and many-relation colon forms,
the objective's `when` and a nested objective, `NORM`, `IN`, scalar subqueries, quoted
identifiers and `DIAGNOSE`. It checks reparsed equality, stable rendering, and binary
`SelectStatement` round trips; a second case does the same for the scope grammar in
depth (deck examples 1–8 as written and fully explicit, words that are keywords inside
DECIDE, `per row` kept as written) and pins its parse errors.

DuckDB's parsed verifier creates and reparses the rendered statement. For a statement
containing DECIDE, that is the end of this verifier: it does not execute a second
solve and compare rows. Two solves may choose different equally optimal assignments,
and an interactive continuation cannot be replayed as a materialized verification
run. The original execution still uses `PRAGMA verify_serializer`; its bound logical
plan is replaced by the binary-deserialized copy before optimization and execution.
Non-DECIDE queries retain DuckDB's existing parsed-result comparison.

The suite guard is:

```bash
DECIDB_VERIFY_SERIALIZER=1 test/decide/.venv/bin/python3 -m pytest test/decide/tests
```

The CLI harness suppresses the pragma's own result, reduces repeated model-dump builds
to one complete dump for shape assertions, and leaves the two PTY-driven interactive
continuation cases outside the materializing verifier. The guarded release run passes
every test.

---

## 6. What this stage does not do

- Decide which side of a comparison a term sits on, or flip a relation.
- Fold a factor into a reducer. `K * AGG(e)` → `AGG(K * e)` is a wrong answer for
  `MIN`/`MAX` under negative `K`, since `MAX(-2x)` is `-2·MIN(x)`, and at the parsed
  level the aggregate kind is not even settled yet. Nothing folds anywhere; the
  canonicalizer peels the factor outward and it stays outside all the way to the
  solver row.
- Any algebraic simplification: no expansion, no like-term collection, no
  constant folding. Products over sums are distributed at extraction time
  (`TryDistributeMultiplyOverAdd`) and coefficients accumulate per solver column
  in the model builder.
- Resolve names, types or table aliases — that is stage 02.

---

## 7. Source map

| Concern | Location |
|---|---|
| Grammar productions, the error-only ones included | `third_party/libpg_query/grammar/statements/select.y` |
| `DIAGNOSE` prefix production | `third_party/libpg_query/grammar/statements/variable_show.y` |
| Conflict budget and rationale | `third_party/libpg_query/grammar/grammar.y` (header comment) |
| Reserved keywords | `third_party/libpg_query/grammar/keywords/reserved_keywords.list` |
| DECIDE `WHEN`, `PER` and `BY` lexer gating | `third_party/libpg_query/src_backend_parser_parser.cpp` (`base_yylex`) |
| Scope carrier and helpers | `third_party/libpg_query/include/nodes/parsenodes.hpp` (`PGDecideScope`), `grammar/grammar.cpp` (`makeDecideScope`, `applyDecideScope`, `makeQualifiedReducer`, `absorbRemovedPerKey`) |
| Clause → `SelectNode` | `src/parser/transform/statement/transform_select_node.cpp` |
| `DIAGNOSE` → `ShowRef` | `src/parser/transform/statement/transform_show_select.cpp` |
| DECIDE tag construction | `src/parser/transform/expression/transform_operator.cpp` |
| Removed spellings refused | `src/parser/decide/decide_removed_spellings.cpp` (`RefuseRemovedDecideSpellings`) |
| Parsed-marker predicates | `src/parser/decide/decide_parsed_markers.cpp` (`IsDecideMarker`, `IsScopedReducerCall`, `TakesReducerBy`, ...) |
| Parsed-tree validation | `src/planner/expression_binder/decide/decide_declarations_binder.cpp` (`BindDeclarations`), `decide_binder.cpp` (`ValidateDecideScopeSpellings` and the other validators) |
| DECIDE statement rendering | `src/parser/query_node/select_node.cpp`, `src/parser/expression/function_expression.cpp` |
| Parsed reparse verifier boundary | `src/verification/parsed_statement_verifier.cpp`, `src/main/client_verify.cpp` |
