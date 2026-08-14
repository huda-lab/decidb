# Stage 01 — Parser

Turns SQL text into a parsed `SelectNode` carrying `decide_variables`,
`decide_constraints`, `decide_sense` and `decide_objective`, then applies the
source-preserving validation that must see what the user actually typed.

**This stage decides no shape.** Which side of a comparison a term sits on, which
way the relation points, and where a factor on a reducer lives are all decided
once by `DecideCanonicalizer` on the bound tree — see
[`../04_canonicalizer/done.md`](../04_canonicalizer/done.md).

---

## 1. Grammar

`third_party/libpg_query/grammar/statements/select.y`. `DECIDE`, `MAXIMIZE` and
`MINIMIZE` are reserved keywords (`grammar/keywords/reserved_keywords.list`), as
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

`typed_decide_variable` accepts exactly three spellings — the type is mandatory:

```
x(TYPE)             row-scoped     — one decision per result row
T.x(TYPE)           table-scoped   — one decision per entity in T
scalar x(TYPE)      query-wide     — one decision for the whole query
```

All three build a `PG_AEXPR_OF` node pairing a `PGColumnRef` (one field, or two
for the qualified form) with the type. Five further alternatives exist **only to
produce an actionable message** rather than a parse failure: a bare `ColId`, a
bare `ColId.ColId`, `scalar T.x(TYPE)` (a contradiction — a query-wide variable
cannot name a table), and the two retired `IS` spellings. Each `ereport`s with the
edit to make, naming the variable.

`decide_constraint_list` likewise rejects comma-separated constraints with
*"use AND between constraints. For multi-column PER, use PER (col1, col2)"*.

### `WHEN` and `PER`

`decide_objective_item` has six alternatives covering `WHEN`, `PER col`,
`PER (col, ...)`, and the `WHEN ... PER ...` combinations. `WHEN` builds
`PG_AEXPR_WHEN_CONSTRAINT`, `PER` builds `PG_AEXPR_PER_CONSTRAINT`, and when both
appear the nesting is `PER(WHEN(body, condition), columns)`.

The aggregate-local form (`SUM(x) WHEN active`) is a separate `c_expr`
alternative on `func_application` and on the relation-qualified reducer form
`func_name '(' func_arg_list ':' func_arg_list ')'`. Constraint-local conditions
stay at `c_expr`: in `SUM(x) WHEN active <= 20`, `active` is the condition and
`<= 20` is the constraint bound. Comparison conditions therefore need
parentheses on that path. Objective WHEN uses `WHEN_DECIDE_OBJECTIVE` and may
consume one comparison between atomic operands because an objective has no
trailing bound. This replaces the former objective parsed-tree reassociation.

`decide_item_expr` closes a constraint or objective body at `DECIDE_ITEM`
precedence. That precedence is above `AND`, so `A AND B WHEN c` and
`A AND B PER col` attach the modifier to `B`, but below `WHEN_DECIDE` and the
comparison operators, so the body and condition retain their SQL expression
shape. No parsed-tree association repair is needed.

### Conflicts

`grammar.y` declares `%expect 6`, itemised in its header comment: 2 inherited
from DuckDB's PostgreSQL-derived postfix-operator states and 4 from the optional
declaration slot, one per `simple_select` alternative. The declaration-slot four
are reachable only when `from_clause` and `where_clause` are both empty, and both
derivations build the same node, so bison's default shift is not load-bearing.
Aggregate-local conflicts for both DECIDE WHEN tokens and the equivalent
qualified-reducer state are resolved explicitly by `DECIDE_ITEM` precedence.

Grammar changes require `make grammar-build` (bison 2.3).

---

## 2. Lexer gating: the DECIDE `WHEN` tokens

`third_party/libpg_query/src_backend_parser_parser.cpp`, in `base_yylex`.

DECIDE's `WHEN` cannot be the global SQL `WHEN`: after a function call it
collided with `WITHIN GROUP` and corrupted ordinary function-call parsing. So
the lexer emits DECIDE-only tokens inside the clause:

- `DECIDE` **or** `SUCH` sets `in_decide_clause` and resets `decide_case_depth`.
  `SUCH` re-arms the flag for the split clause order, where the declaration slot
  cleared it so `FROM` / `JOIN ... ON` / `WHERE` lex as ordinary SQL.
- Grammar actions clear the flag once the clause is closed.
- While armed, `CASE` increments `decide_case_depth` and `END` decrements it, so a
  `CASE ... WHEN ... END` keeps its ordinary `WHEN` and still parses.
- A depth-0 `WHEN` becomes `WHEN_DECIDE` in constraints. After `MAXIMIZE` or
  `MINIMIZE`, `in_decide_objective` makes it `WHEN_DECIDE_OBJECTIVE`, allowing
  the grammar to distinguish a condition comparison from a constraint bound.

No lookahead is needed for this decision.

When a parse still fails, `MaybeAppendDecideWhenHint` (`src/decidb/utility/decide_parse_hints.cpp`,
called from `src/parser/parser.cpp:229`) appends a DECIDE-specific hint to the
error.

---

## 3. Transformer

`src/parser/transform/statement/transform_select_node.cpp:105-119` moves the
clause onto the `SelectNode`: `decide_variables` (via `TransformExpressionList`),
`decide_constraints`, `decide_sense` (`MAXIMIZE` / `MINIMIZE` / `FEASIBILITY`
when no objective was written), and `decide_objective`.

`src/parser/transform/expression/transform_operator.cpp:210-237` converts the two
DECIDE `PG_AEXPR` node types into tagged `FunctionExpression`s with
`is_operator = true`:

| Parsed node | Becomes |
|---|---|
| `PG_AEXPR_WHEN_CONSTRAINT` | `FunctionExpression(WHEN_CONSTRAINT_TAG, [body, condition])` |
| `PG_AEXPR_PER_CONSTRAINT` | `FunctionExpression(PER_CONSTRAINT_TAG, [body, col...])` |

Those two tags are how `WHEN` and `PER` ownership travels the rest of the
pipeline. Every later stage recurses into child 0 only and copies the condition or
grouping columns unchanged.

---

## 4. Parsed-tree preparation

These run on the parsed tree before DECIDE binding. They physically live in
`src/planner/binder/query_node/bind_select_node.cpp` because the binder is the
first consumer of the clause.

In execution order:

| # | Operation | What it does |
|---|---|---|
| 1 | `RewriteScopedVarRefs` | Strips the table qualifier from `T.x` where `T.x` names a registered scoped variable, so constraints, objective and the SELECT list all see bare `x`. Every variable is registered under its unqualified name, and duplicates are rejected, so the bare form always resolves. |
| 2 | `ValidateDecideNoExplicitDecisionCasts` | Rejects `CAST`, `TRY_CAST` and `::` whose child subtree contains a decision reference, in `SUCH THAT` and the objective. Target type is irrelevant. Must run here: a source cast and a binder cast are both `BoundCastExpression`, so authorship cannot be recovered later. The post-solve `SELECT` projection is not scanned. |
| 3 | `ValidateDecideNoNonLinearScalar` | Rejects non-linear use of a query-wide `scalar` variable before anything downstream has to interpret it. |
| 4 | `TagDecideSourceFragments` | Stamps each source fragment with an id so diagnostics can quote the user's own SQL after the tree has been rebuilt. |

`norm()` and DECIDE-variable `IN (...)` are deliberately not rewritten here.
The binder retains bound markers, and stage 05 chooses the indicator and linking
formulation after types, scopes, and DuckDB coercions are known.

---

## 5. What this stage does not do

- Decide which side of a comparison a term sits on, or flip a relation.
- Fold a factor into a reducer. `K * AGG(e)` → `AGG(K * e)` used to happen here
  and was a wrong answer for `MIN`/`MAX` under negative `K`, since `MAX(-2x)` is
  `-2·MIN(x)`. At the parsed level the aggregate kind is not even settled yet.
  Nothing folds anywhere now; the canonicalizer peels the factor outward and it
  stays outside all the way to the solver row.
- Any algebraic simplification: no expansion, no like-term collection, no
  constant folding. Products over sums are distributed at extraction time
  (`TryDistributeMultiplyOverAdd`) and coefficients accumulate per solver column
  in the model builder.
- Resolve names, types or table aliases — that is stage 02.

---

## 6. Source map

| Concern | Location |
|---|---|
| Grammar productions | `third_party/libpg_query/grammar/statements/select.y` |
| Conflict budget and rationale | `third_party/libpg_query/grammar/grammar.y` (header comment) |
| Reserved keywords | `third_party/libpg_query/grammar/keywords/reserved_keywords.list` |
| DECIDE `WHEN` lexer gating | `third_party/libpg_query/src_backend_parser_parser.cpp` (`base_yylex`) |
| Clause → `SelectNode` | `src/parser/transform/statement/transform_select_node.cpp` |
| `WHEN`/`PER` tag construction | `src/parser/transform/expression/transform_operator.cpp` |
| Parse-error hint | `src/decidb/utility/decide_parse_hints.cpp` |
| Parsed-tree rewrite call sites | `src/planner/binder/query_node/bind_select_node.cpp` |
