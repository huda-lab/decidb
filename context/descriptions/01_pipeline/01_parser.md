# Parser & Symbolic Layer

## 1. Overview
The Parser and Symbolic Layer is the entry point for the `DECIDE` clause. Its primary responsibility is not just to build a parse tree, but to **normalize** the user's algebraic expressions into a canonical form that the system can optimize. This is a critical step because SQL allows flexible expression shapes (e.g., `x * 2 + 5`), whereas linear solvers require a strict `coeff * variable` structure.

**Key Source File**: `src/decidb/symbolic/decide_symbolic.cpp`

## 2. Symbolic Translation

> **Note on IN/BETWEEN**: `IN` and `BETWEEN` are handled as symbolic predicate types during parsing. The symbolic layer passes them through unchanged — they are validated or rewritten at the binder stage, not during normalization. `IN` on decision variables is supported via `RewriteInDomain()` in `bind_select_node.cpp`, which rewrites `x IN (v1, ..., vK)` into K binary indicator variables with cardinality and linking constraints. `IN` on aggregates (e.g., `SUM(x) IN (...)`) remains unsupported.

DecidB integrates `SymbolicC++` to perform algebraic manipulations. The translation pipeline is as follows:

1.  **DuckDB to Symbolic**: The `ToSymbolicRecursive` function traverses the DuckDB `ParsedExpression` tree.
    -   `ColumnRef` (decision variable) $\rightarrow$ `Symbolic Variable`
    -   `ColumnRef` (normal column) $\rightarrow$ `Symbolic Constant` (treated as opaque for now)
    -   `Operator` (+, -, *) $\rightarrow$ `Symbolic Operation`

2.  **Normalization**: The symbolic engine simplifies the expression. This involves:
    -   Expanding parentheses: `2 * (x + 5)` $\rightarrow$ `2x + 10`
    -   Collecting like terms: `x + x` $\rightarrow$ `2x`
    -   Separating constants.

3.  **Symbolic to DuckDB**: The `FromSymbolic` function converts the simplified symbolic expression back into a DuckDB `ParsedExpression`, structured specifically for the Binder.

## 3. Canonical Forms

The parser ensures that all constraints and objectives are rewritten into the following canonical forms before they reach the Binder.

### 3.1 Constraints
All constraints are normalized to:
$$ \sum (c_i \cdot x_i) \leq K - \sum (RowTerm_j) $$
Where:
-   $x_i$: Decision variables.
-   $c_i$: Coefficients (can be expressions).
-   $K$: A constant.
-   $RowTerm_j$: Terms involving only table columns (no decision variables).

**Example Transformation**:
Input SQL:
```sql
SUM(profit * x - cost) <= 500
```
Internal Steps:
1.  Symbolic: $\sum (P \cdot x - C) \leq 500$
2.  Split Sum: $\sum (P \cdot x) - \sum C \leq 500$
3.  Rearrange: $\sum (P \cdot x) \leq 500 + \sum C$

The Parser rewrites the expression tree so that the LHS contains **only** decision-dependent terms, and the RHS contains **only** scalar terms.

### 3.2 Objectives
Objectives are similarly normalized to:
$$ \text{MAX/MIN } \sum (c_i \cdot x_i) $$
Constant offsets in the objective (e.g., `MAX SUM(x * p + 10)`) are dropped from the optimization problem as they do not affect the optimal choice of $x$, though they are technically preserved in the final projection if needed.

### 3.3 Normalizer Path Architecture

The unified canonical form above describes the *output* of normalization. The *implementation* is split across four mutually-exclusive paths in `NormalizeComparisonExpr`. Each exists because the SymEngine-backed default path is destructive for an LHS class that carries DECIDE-specific structural tags SymEngine doesn't understand. The paths are guarded by `if (...) return cmp.Copy()` early returns and run in this order — first match wins:

| # | Path | LHS shape | Why SymEngine breaks it | Downstream consumer |
|---|---|---|---|---|
| 1 | Quadratic LHS bypass | `SUM(POWER(linear, 2))`, `SUM((expr)*(expr))` with same vars | `.expand()` would distribute the square and lose the recognizable `POWER(linear, 2)` pattern | QP extractor in `physical_decide.cpp::DetectQuadraticPattern` |
| 2 | Bilinear LHS bypass | `SUM(x * y)` with disjoint decide vars | `.expand()` would distribute and reorder factors; McCormick extractor matches on specific tree shapes | Bilinear path in `physical_decide.cpp` |
| 3 | Composed MIN/MAX bypass | `SUM(...) + MIN(...)`, `SUM(...) + MAX(...)` etc. | SymEngine has no MIN/MAX semantics — it would treat them as opaque function symbols and combine incorrectly with surrounding additive terms | `RewriteComposedMinMaxInConstraint` in the optimizer |
| 4 | Aggregate-local WHEN path | LHS contains `SUM(...) WHEN c` (or AVG/MIN/MAX WHEN) | SymEngine has no WHEN semantics — it would absorb the WHEN tag as opaque, then expand around it, scrambling which terms the per-aggregate filter applies to | Per-row aggregate-LHS extractor (after parsed-level rewrite, see below) |
| 5 | **Default path** | Plain linear LHS | n/a (this is what SymEngine handles cleanly) | Per-row aggregate-LHS extractor |

**Path 4 isn't a true bypass** — it does its own parsed-level rewrite instead of falling through unchanged:

1. `CopyAndFoldConstantsIntoWhenAggregates` recursively rewrites `K * (SUM(...) WHEN c)` and `(SUM(...) WHEN c) / K` into `WHEN(SUM(K*x), c)` (and `WHEN(SUM(x/K), c)`) so the extractor sees a bare WHEN-tagged aggregate.
2. `DecomposeAdditiveAtParsed` walks the LHS through `+`, binary `-`, unary `-`, CAST. Sign-tracks. Pure-numeric leaves accumulate into `lhs_offset`; everything else becomes a sign-tagged structural term.
3. `BuildAdditiveExpressionFromTerms` rebuilds the LHS without the constants.
4. Returns `LHS_struct OP (RHS - lhs_offset)`. The QP/bilinear/composed-MIN structures inside the WHEN-tagged aggregates pass through untouched, so a WHEN-bearing LHS that *also* contains POWER or `x*y` is handled correctly even though path 4 fires before paths 1/2 would have.

#### Safety invariant

The bypass conditions are **not disjoint** — practical queries can match multiple (e.g. `(SUM(POWER(x,2)) WHEN c) + 3 <= K` matches paths 1 and 4). The first-match-wins ordering is safe because each path is conservative: it preserves arbitrary leaf subtrees as opaque units when it doesn't recognize them. Paths 1–3 return `cmp.Copy()` (no rewrite at all); path 4 only rewrites recognized shapes (constant additive offsets, `K * WHEN`, `WHEN / K`) and leaves everything else as opaque structural terms.

This invariant is pinned down by oracle-verified cross-product integration tests in `test/decide/tests/test_normalizer_path_interactions.py` covering: quadratic + WHEN, quadratic + WHEN + offset, bilinear + WHEN, bilinear + WHEN + offset, and composed SUM + MIN + WHEN.

#### Refactor tripwire

The four-path structure is tolerable but smelly — every bypass exists because SymEngine destroys structure the downstream pipeline needs. A fifth bypass is the signal to refactor to a single classification-driven normalizer: classify the LHS once into `{ Constant, DataScalar, Aggregate, AggregateWithWhen, QpBase, BilinearProduct, PerRowVariable }`, then dispatch each leaf type to a dedicated handler in one walker. That refactor would also eliminate the SymEngine dependency, since its only remaining value-add (`expand()` distribution + `simplify()` like-term combination) is implementable with the same parsed-level walker that already powers paths 3 and 4.

Until then, the architecture comment at the top of `src/decidb/symbolic/decide_symbolic.cpp` is the authoritative reference for the path ordering and helpers.

## 4. Interaction with Binder
The Binder receives this normalized tree. It no longer needs to perform algebraic rearrangement; it simply validates that the structure matches the expectation (linear sum on LHS, scalar on RHS) and binds the column references.

## 5. `WHEN` Keyword

The parser handles the expression-level `WHEN` postfix keyword for conditional constraints and objectives via DECIDE-scoped grammar rules. The whole-expression form lives in the dedicated `decide_constraint_item` and `decide_objective_item` non-terminals:

```yacc
decide_constraint_item:
    a_expr WHEN a_expr    /* constraint WHEN condition */
    | a_expr              /* unconditional constraint */
    ;
```

The parser emits a `PG_AEXPR_WHEN_CONSTRAINT` node, which the transformer converts to a `FunctionExpression("__when_constraint__", [constraint, condition])`. The symbolic layer normalizes the constraint child while passing through the condition unchanged.

> **Note on normalization with wrappers**: Normalization passes through PER and WHEN wrappers unchanged — only the inner constraint expression is normalized. The wrapper functions (`__per_constraint__`, `__when_constraint__`) are preserved as-is around the normalized child.

Aggregate-local `WHEN` uses the same parser tag but a different grammar entry:

```yacc
c_expr:
    func_application WHEN decide_when_condition
    | ...

decide_when_condition:
    c_expr
```

This permits additive aggregate expressions such as:

```sql
SUM(x * w) WHEN active + SUM(x * w2) WHEN priority <= 10
```

The condition is intentionally narrow (`c_expr`) so the aggregate-local form does not consume the rest of a DECIDE expression. Comparison predicates for aggregate-local filters therefore need parentheses, for example `SUM(x * w) WHEN (category = 'A')`. Legacy objective syntax such as `MAXIMIZE SUM(x) WHEN category = 'A'` is normalized in `NormalizeDecideObjective()` back into a whole-objective `WHEN` to preserve existing semantics.

## 6. `PER` Keyword (Grouped Constraints)

The `PER` keyword uses a similar grammar scoping approach as `WHEN`. It lives in the same `decide_constraint_item` non-terminal rather than the global `a_expr` production:

```yacc
decide_constraint_item:
    a_expr PER a_expr WHEN a_expr    /* constraint PER column WHEN condition */
    | a_expr PER a_expr              /* constraint PER column */
    | a_expr WHEN a_expr             /* constraint WHEN condition */
    | a_expr                         /* unconditional constraint */
    ;
```

The parser emits a `PG_AEXPR_PER_CONSTRAINT` node (analogous to `PG_AEXPR_WHEN_CONSTRAINT`), which the transformer converts to a `FunctionExpression("__per_constraint__", [constraint, per_column])`.

The symbolic layer normalizes the constraint child while passing through the PER column unchanged.

**Combined PER+WHEN**: When both keywords are present, the wrapper nesting is `PER(WHEN(constraint, condition), per_column)`. PER wraps the outer layer, and WHEN wraps the inner constraint+condition pair.

## 7. Table-Scoped Decision Variables

Table-scoped (entity-scoped) decision variables use a qualified `table.variable` syntax, parsed via a dedicated grammar rule in `select.y`:

```yacc
ColId '.' ColId IS variable_type
```

When the parser encounters a dotted name in the `DECIDE` clause (e.g., `DECIDE drivers.assigned IS BOOLEAN`), it produces a qualified `PGColumnRef` with a two-part name list — the first part is the table alias, the second is the variable name. This is the same `PGColumnRef` structure DuckDB uses for qualified column references (e.g., `t.col`), so no new AST node types are required.

The symbolic layer treats the qualified variable name as an opaque identifier during normalization, preserving the table prefix through algebraic simplification. The binder (not the parser) is responsible for resolving the table alias and validating that the referenced table exists in the query's bind context.
