# Stage 01 — Parser

Turns SQL text into a parsed `SelectNode` carrying `decide_variables`,
`decide_constraints`, `decide_sense` and `decide_objective`, and applies the
source-preserving validation that must see what the user actually typed. A form the grammar
does not have, such as a removed spelling, is an ordinary syntax error.

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
second `DECIDE` arriving through `decide_body` reports *"DECIDE appears twice"*, at the second `DECIDE`, instead of
a generic syntax error. A `SUCH THAT` with no declaration is *"SUCH THAT has no DECIDE clause"*, at the `SUCH THAT`.

A declaration with no `SUCH THAT` is refused by the transformer
(`DECIDE_NO_CONSTRAINTS_MESSAGE`: *"DECIDE has no SUCH THAT clause"*, at the `DECIDE`), not by a grammar action: Bison reduces by default, so
an action runs before the parser reads the next token, and an error raised there would
hide a syntax error at that token. `DECIDE x(INT) y(INT) FROM t SUCH THAT ...` therefore
reports `syntax error at or near "y"`. Only an objective where the constraints belong is
refused in the grammar, by `decide_tail`'s `MAXIMIZE` and `MINIMIZE` alternatives, since
without them it would be a syntax error at the word the message can explain.

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
a `per`, `applyDecideScope` wraps the name in its scope marker.

A name with no type (`x`, or `per K: x`) is the same declaration node with a NULL type, as DuckDB's `CREATE TABLE` builds
a column with no type. Bison reduces by default,
so an action in that production would run before the parser reads the token after the
name, and its error would hide a syntax error there (`DECIDE t.x(INT) ...`). The
transformer refuses the NULL type once the whole query has parsed, at the name: `decision
'x' has no type` (`../../language/errors.md`). `scalar` is an ordinary name, so it names
a column or a decision (`DECIDE scalar(INT)`).

A declaration with no type builds the declaration node with a NULL type, as DuckDB's `CREATE TABLE` does for a column
with no type; the transformer refuses a node whose `rexpr` is null.

The type in the parentheses is a `ColId`, which `makeDecideVariableType` (`grammar.cpp`)
reads as `INT`, `REAL` or `BOOL` in any case; any other name is a syntax error at it.
DECIDE adds no keyword for a type: DuckDB quotes every keyword when it prints a name, so
a keyword `BOOL` would quote a column or a `UNION` member named `bool` in every message
and rendered query.

`decide_constraint_list` joins its items with `AND`. A comma between constraints is a plain syntax error.

### Scopes: `when`, `per`, `by`

A scope is written *in front* of what it governs (../../language/guide.md §1.1):

```
decide_scope  ::= decide_when | decide_when decide_per | decide_per
decide_when   ::= WHEN_DECIDE a_expr
decide_per    ::= PER_DECIDE expr_list | PER_DECIDE_PAREN '(' ')' | PER_DECIDE_ROW ROW
decide_by     ::= BY_DECIDE '(' ')' | BY_DECIDE '(' expr_list ')'
```

- `decide_constraint_item` and `decide_objective_item` are each
  `decide_scope ':' decide_item_expr` or the bare `decide_item_expr`. A
  `decide_scope ':'` may also precede the first argument of a function call
  (`func_name '(' decide_scope ':' func_arg_list ')'`), and `decide_per ':'` a
  declared name. `decide_by` may follow a `func_application` or a scoped call.
- The scope is carried through the grammar as a `PGDecideScope` (`parsenodes.hpp`)
  and folded by `applyDecideScope` (`grammar.cpp`) into `PG_AEXPR_SCOPE_WHEN`
  (`lexpr` body, `rexpr` condition) inside `PG_AEXPR_SCOPE_PER` (`lexpr` body,
  `rexpr` the key list, `NULL` for `per ()`). `per row`, the default written out,
  builds `PG_AEXPR_SCOPE_PER_ROW` (`lexpr` body, no key), so a clause keeps the
  spelling it was written in (D19). `by` builds `PG_AEXPR_REDUCER_BY` (`lexpr` the
  call, `rexpr` the key list, `NULL` for `by ()`).
- A key element must be a column or relation name (`decideCheckKey`): anything
  else is *"'q' in a per key is not a column or a relation"*, at the element.
  A parenthesized key is a plain syntax error. A `per`
  other than `()` on an objective is *"an objective cannot have a per key"*, at the
  `per`. A `WHEN` or `PER` written after a clause or a reducer is a plain syntax error,
  as is one written after a clause that already has a scope in front.
- Because the colon ends the scope, the condition is a full `a_expr` and needs no
  parentheses.
- `decide_item_expr` closes a constraint or objective body at `DECIDE_ITEM`
  precedence. That precedence is above `AND`, so a top-level `AND` separates
  constraints and `per g: A AND B` scopes only `A`, but below the comparison
  operators and the DECIDE `WHEN` tokens, so the body keeps its SQL expression shape.
  No parsed-tree association repair is needed.

### Frame selectors

`at(F [else v]: e) over (S ...)` and `agg(from F1 to F2 [every d] [else v | all]: e) over (S ...)`
(`../../language/guide.md` §5.6) are `c_expr` alternatives beside the scoped reducers:

```
c_expr              += AT_DECIDE '(' decide_frame_point [ELSE a_expr] ':' a_expr ')' decide_frame_over
                     | func_name '(' FROM decide_frame_point TO decide_frame_point
                       opt_decide_frame_every [ELSE a_expr | ALL] ':' func_arg_list ')' decide_frame_over
decide_frame_point  ::= FIRST_P | LAST_P | NEXT | IDENT | Iconst NEXT | Iconst IDENT
decide_frame_over   ::= OVER_DECIDE '(' a_expr opt_asc_desc [IDENT] [WITHIN_DECIDE ( '(' ')' | expr_list )] ')'
```

- `previous`, `every` and `cyclic` are `IDENT`s whose text the grammar checks whatever its case, as a keyword's
  (`makeDecideFramePrevious`, `decideFrameWord`), so they become no keyword and no column anywhere loses the name:
  *"'prev' is not a frame position"*, *"'cyclc' is not a frame keyword"*, each at the word. A misspelling that is itself a
  keyword or a token the grammar cannot place (`each`, `cycle`, `evry`) is a plain syntax error.
- A `within` element must be a column or relation name (`decideCheckKey`): *"'q' in within is not a column or a
  relation"*, at the element.
- The frame travels as a `PGDecideFrame` (`parsenodes.hpp`): the anchors and their `k`, `every`, `all`, the
  fallback, the body (the payload, or for a range the call `agg(args)`), the order key, `desc`, `cyclic`, the
  `within` list.
- A SQL window written at the clause's level arrives with `OVER_DECIDE` (§2), so `over_clause` takes `OVER_DECIDE
  window_specification` and `OVER_DECIDE ColId` too: `when row_number() over () > 1: ...` parses as before and the
  binder refuses it where it did.

### Conflicts

`grammar.y` declares `%expect 4`, itemised in its header comment: all four come
from the optional declaration slot, one per `simple_select` alternative. They are
reachable only when `from_clause` and `where_clause` are both empty, and both
derivations build the same node, so bison's default shift is not load-bearing.
The `WHEN_DECIDE` conflict of the empty `within_group_clause` is resolved explicitly by
`DECIDE_ITEM` precedence, below both DECIDE `WHEN` tokens, so the parser shifts `WHEN`;
it is keyed on tokens `base_yylex` emits only inside DECIDE. `PER` and a reducer's `BY` reach the grammar as DECIDE-only tokens, so
neither can be read as an identifier there. The frame selector productions add no
conflict: `AT_DECIDE`, `OVER_DECIDE` and `WITHIN_DECIDE` are DECIDE-only tokens too, and
a range is told from a call by the `FROM` after its `(`.

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
- A depth-0 `WHEN` becomes `WHEN_DECIDE`, which the scope grammar reads (`decide_when`).
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
- **Frame selectors** take three more tokens, emitted only at the clause's own level: `AT` before `(` becomes
  `AT_DECIDE` (the `PER` lookahead; `AT TIME ZONE` keeps `AT`), `OVER` directly after `)` becomes `OVER_DECIDE`, and
  `WITHIN` directly inside that `OVER (...)` becomes `WITHIN_DECIDE` (so `WITHIN GROUP` elsewhere is untouched). "The
  clause's own level" is the body after `SUCH THAT` only (`decide_in_body`, so a decision may be declared
  `at(REAL)`), outside every SQL subquery written there. `decide_paren_depth` counts parentheses over the whole
  statement; a subquery opens at `decide_subquery_depth` and lasts until the parenthesis holding it closes. It opens at
  a `SELECT`, or at a `WITH`, `VALUES`, `TABLE`, `PIVOT` or `UNPIVOT` directly after `(`, or at a `FROM` there unless a
  name comes before the `(` (`sum(from first ...)` is a range frame, `(FROM t ...)` a query). `decide_over_parens`
  has bit d set while the parenthesis at depth d is a frame's `OVER_DECIDE` one, so a window or frame inside an order
  key leaves the outer `within` its own. A clause nested in a subquery saves and restores its subquery depth and
  `decide_in_body` with the rest of its state. A declaration with no `FROM` after it (`FROM t DECIDE x(INT) SUCH
  THAT ...`, `SELECT x DECIDE x(INT) SUCH THAT ...`) reduces only once `SUCH` is read as its lookahead, which has
  opened the body already: its action drops the state it saved instead of restoring it (`PGDecideDropLexState`), or
  the body would lex as the declaration list and read no frame. So a window, a named window or a `WITHIN GROUP` inside a subquery
  written in the clause keeps its SQL meaning, and so does a function called `at` there; at the clause's level it is
  written quoted, `"at"(...)`.

Inside a DECIDE clause, subqueries written there included, these words are
therefore keywords in those positions, never names: a column called `per`, a `by`
after `)` or a `row` after `per` is written quoted there (`"per"`), as the printer
does. Outside the clause nothing changes.

---

## 3. Transformer

`TransformSelectNode` (`src/parser/transform/statement/transform_select_node.cpp`) moves the
clause onto the `SelectNode`: `decide_variables` (via `TransformExpressionList`),
`decide_constraints`, `decide_sense` (`MAXIMIZE` / `MINIMIZE` / `FEASIBILITY`
when no objective was written), and `decide_objective`.

A frame selector is not a tagged function but a parsed expression class of its own, `DecideFrameExpression`
(`ExpressionClass::DECIDE_FRAME`, `src/include/duckdb/parser/expression/decide_frame_expression.hpp`), the way
`WindowExpression` is: `reducer` (empty for a point frame), `from`/`from_k`, `to`/`to_k`, `every`, `all`, `payload`,
`fallback`, `order`, `descending`, `cyclic`, `within`. `Transformer::TransformDecideFrame`
(`transform_decide_frame.cpp`) builds it. A range's reducer is read from the call as a name and one argument, never
through `TransformFuncCall`, which turns some names (`construct_array`, `if`) into other kinds of expression:
*"a range frame reduces one expression, 'sum' has 2"*, at the reducer, and *"an expression in a frame cannot have a
name"* for `a := e`, at the frame. It refuses a `k` or `d` of 0, as DuckDB refuses `#0`: *"'0' in a frame is not a
positive whole number"*, at the frame. Its children (payload, fallback, order key, `within` elements) are visited by
`ParsedExpressionIterator`, so every parsed-tree walk sees inside a frame. `DecideFrameExpression::ToString()` prints a
frame in lower case with its defaults left out (`asc`, `k` = 1, `every 1`, `within ()`), the text a message quotes;
the statement itself prints the clause as typed, and a frame with its defaults written equals one without.

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

All four reach the binder, which binds every one into the bound tree's own
wrappers and tags (`../02_binder/done.md` §4a).

**Source spans.** The grammar wraps every expression of a DECIDE clause in one decorator reduction
(`a_expr: a_expr_body` and `b_expr: b_expr_body` call `noteDecideExpr`), the one place that sees an expression with its
text. It records `{node, start, end}` with the role `EXPR`; `noteDecideParen` leaves the parentheses of `( e )` out of
`e`'s span. The other roles are the constructs that are no expression: `PER`, `BY`, the two halves of a `BETWEEN` and
the objective. The interceptor `Transformer::TransformExpression` (Clang's
SourceManager idea: one store of text, a location per node) keys each `EXPR` span by the query location its parsed
expression took, innermost first, together with a `DecideBinding`: how tightly the node's own text binds (`WHOLE` for a
call, column, literal, `::` cast, parenthesized text, subquery and `CASE ... END`; `LOOSE`, `ADDITIVE`, `MULTIPLICATIVE`
or `POWER` for an operator). `TransformSelectNode` assembles the clause's text, from `DECIDE` through the objective, and
its spans into `SelectNode::decide_text` (`DecideSourceText`): one buffer with two marks. `start`, the `DECIDE`, is the
base of every offset; `tail_start`, the `SUCH`, anchors the objective and begins `Tail()`, which `SelectNode::ToString`
prints after the declarations. A declaration's `per` key has its span as a clause's does, so a message quotes it as
written. In the split order the buffer holds the `FROM` and `WHERE` text between the declarations and `SUCH THAT`,
which no span covers and nothing cuts. A parameter takes its location too, so `$1` is quoted as written.

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
| 1 | `RefuseQualifiedDecisionReferences` | Refuses a decision referenced through a relation, `D.x`, in a `when`, a key or a `by` (`../02_binder/done.md` §1). |
| 2 | `ValidateDecideScopeSpellings` | Where `when`, `per` and `by` may stand (`../02_binder/done.md` §4a). |
| 3 | `ValidateDecideNoExplicitDecisionCasts` | Rejects a user-written cast over a decision, in `SUCH THAT` and the objective; only the parsed tree shows who wrote a cast (`../04_canonicalizer/done.md` §3.6). |

Every refusal of algebra is the shape check's (`../04_canonicalizer/done.md` §3.11).

`norm()` and DECIDE-variable `IN (...)` are deliberately not rewritten here.
The binder retains bound markers, and stage 05 chooses the indicator and linking
formulation after types, scopes, and DuckDB coercions are known.

---

## 5. Statement rendering and parsed serialization

The parsed tree stores DECIDE syntax as private structure: a variable declaration is
a comparison against a type-marker string, and a scope and a `by` are
tagged `FunctionExpression`s. Those encodings are for the parser and binder, not SQL
users.

`SelectNode::ToString()` reverses the encoding. It emits `x(INT)`, `PER key: x(INT)`,
`PER (): x(REAL)` and `PER ROW: x(BOOL)`, renders the top-level constraint list without
generic SQL parentheses that the DECIDE grammar cannot accept, and restores objective
direction. `FunctionExpression::ToString()` restores the scope `WHEN condition PER key:
body` (WHEN first, as written, although PER is the outer tag), `PER ROW: body`
and `reducer BY (key)` wherever those tags occur inside a larger
expression. Ordinary parsed functions keep DuckDB's generic renderer.
`SelectNode::decide_sense` defaults to `FEASIBILITY` and `decide_text` to empty, and
`query_node.json` gives both that default, so they are written only for a DECIDE query
and a view DuckDB stored, which has neither, loads with the defaults.

This is a structural round trip, not source-text preservation: DuckDB may normalize
case, quotes, parentheses, or the accepted split clause order. The contract is:

```text
SQL -> parsed SelectStatement -> ToString SQL -> equal parsed SelectStatement
```

Parsed-statement binary serialization is exercised separately from logical-plan
serialization. The regression matrix in
`test/common/test_decidb_plan_serialization.cpp` covers every variable type and
declaration scope, both clause orders, a constraint's `when` with a multi-column `per`
and a `by`, a reducer's own `when` and `per` with one relation and with several in its key,
the objective's `when` and nested form, `NORM`, `IN`, scalar subqueries, quoted
identifiers and `DIAGNOSE`. It checks reparsed equality, stable rendering, and binary
`SelectStatement` round trips; a second case does the same for the scope grammar in
depth (deck examples 1–8 as written and fully explicit, words that are keywords inside
DECIDE, `per row` kept as written) and pins its parse errors.

DuckDB's parsed verifier creates and reparses the rendered statement. For a statement
containing DECIDE, that is the end of this verifier: it does not execute a second
solve and compare rows. Two solves may choose different equally optimal assignments,
and an interactive continuation cannot be replayed as a materialized verification
run. The original execution still uses `PRAGMA verify_serializer`; its bound logical
plan is replaced by the binary-deserialized copy before optimization and execution
(the suite guard is in [`../03_logical_plan/done.md`](../03_logical_plan/done.md) §5).
Non-DECIDE queries retain DuckDB's existing parsed-result comparison.

---

## 6. What this stage does not do

- Decide which side of a comparison a term sits on, or flip a relation.
- Fold a factor into a reducer. At the parsed level the aggregate kind is not even
  settled yet, and nothing folds anywhere: the canonicalizer peels the
  factor outward and it stays outside all the way to the solver row
  (`../04_canonicalizer/done.md` §3.7).
- Any algebraic simplification: no expansion, no like-term collection, no
  constant folding. A coefficient on a sum is distributed over its terms when
  stage 05 reads them (`DecideTermSplitter`), and coefficients accumulate per solver
  column in the model builder.
- Resolve names, types or table aliases — that is stage 02.

---

## 7. Source map

| Concern | Location |
|---|---|
| Grammar productions | `third_party/libpg_query/grammar/statements/select.y` |
| `DIAGNOSE` prefix production | `third_party/libpg_query/grammar/statements/variable_show.y` |
| Conflict budget and rationale | `third_party/libpg_query/grammar/grammar.y` (header comment) |
| Reserved keywords | `third_party/libpg_query/grammar/keywords/reserved_keywords.list` |
| DECIDE `WHEN`, `PER`, `BY`, `AT`, `OVER` and `WITHIN` lexer gating | `third_party/libpg_query/src_backend_parser_parser.cpp` (`base_yylex`), state in `include/parser/gramparse.hpp` |
| Frame carrier and helpers | `parsenodes.hpp` (`PGDecideFrame`), `grammar/grammar.cpp` (`makeDecideFrameOver`, `makeDecideFramePoint`, `makeDecideFrameRange`) |
| Frame parsed expression and its transform | `src/parser/expression/decide_frame_expression.cpp`, `src/parser/transform/expression/transform_decide_frame.cpp` |
| Scope carrier and helpers | `third_party/libpg_query/include/nodes/parsenodes.hpp` (`PGDecideScope`), `grammar/grammar.cpp` (`makeDecideScope`, `applyDecideScope`) |
| Source spans (one per expression of the clause, a `per`/`by` key, a BETWEEN's halves, the objective) | `grammar/grammar.cpp` (`noteDecideExpr`, `noteDecideParen`, `noteDecideReducerKey`, `noteDecideSpan`), called from the `a_expr` and `b_expr` decorator reductions in `select.y`; the roles are `PGDecideSpanRole` and `DecideSpanRole`, and `language/explain.md` describes how they are used |
| Span keyed by query location, with its `DecideBinding` | `src/parser/transform/expression/transform_expression.cpp` (the interceptor `Transformer::TransformExpression`), `transform_select_node.cpp` (assembles `DecideSourceText`) |
| Clause → `SelectNode` | `src/parser/transform/statement/transform_select_node.cpp` |
| `DIAGNOSE` → `ShowRef` | `src/parser/transform/statement/transform_show_select.cpp` |
| DECIDE tag construction | `src/parser/transform/expression/transform_operator.cpp` |

| Parsed-marker predicates | `src/parser/decide/decide_parsed_markers.cpp` (`IsDecideMarker`, `IsScopedReducerCall`, `TakesReducerBy`, ...) |
| Parsed-tree validation | `src/planner/expression_binder/decide/decide_declarations_binder.cpp` (`BindDeclarations`), `decide_binder.cpp` (`ValidateDecideScopeSpellings` and the other validators) |
| DECIDE statement rendering | `src/parser/query_node/select_node.cpp`, `src/parser/expression/function_expression.cpp` |
| Parsed reparse verifier boundary | `src/verification/parsed_statement_verifier.cpp`, `src/main/client_verify.cpp` |
