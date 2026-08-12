# Parser & Symbolic Layer

## 1. Overview
The Parser and Symbolic Layer is the entry point for the `DECIDE` clause. It builds the parse tree and then does two things to it, on the **parsed** expression, before the DECIDE-specific binders run:

- **Objective simplification.** A `SUM` objective body is expanded and simplified with SymbolicC++, then rebuilt factored as `coefficient * decide_part` per term. SQL allows flexible expression shapes (`x * 2 + 5`) where solvers want a strict `coeff * variable` structure.
- **Grammar repair.** Precedence mis-parses that the grammar cannot express directly are fixed here — `A AND B WHEN c` and `MAXIMIZE SUM(x) WHEN a > 5` both parse the wrong way round and are reassociated.

**It no longer decides constraint shape.** Which side of a comparison each term sits on is `DecideCanonicalizer`'s job, on the bound tree — see §4. This layer used to do it too, in a second implementation; that half was deleted at `canonicalize.md` phase C.4 (2026-08-12) after measurement showed it changed no model and no result.

**Key Source File**: `src/decidb/symbolic/decide_symbolic.cpp`

## 2. Symbolic Translation

> **Note on IN/BETWEEN**: `IN` and `BETWEEN` are handled as symbolic predicate types during parsing. The symbolic layer passes them through unchanged — they are validated or rewritten at the binder stage, not during normalization. `IN` on decision variables is supported via `RewriteInDomain()` in `bind_select_node.cpp`, which rewrites `x IN (v1, ..., vK)` into K binary indicator variables with cardinality and linking constraints. `IN` on aggregates (e.g., `SUM(x) IN (...)`) remains unsupported.

DeciDB integrates `SymbolicC++` to perform algebraic manipulations. The translation pipeline is as follows:

1.  **DuckDB to Symbolic**: The `ToSymbolicRecursive` function traverses the DuckDB `ParsedExpression` tree.
    -   `ColumnRef` (decision variable) $\rightarrow$ `Symbolic Variable`, named by the **unqualified** variable name. `bind_select_node.cpp` always registers that form (the qualified `T.x` spelling is an alias for the same index), so `keepS` and `S.keepS` canonicalize to one symbol and the `decide_variables.count(name)` classification used by `SymbolicContainsDecideVariable` / `CollectDecideFactors` keeps working.
    -   `ColumnRef` (normal column) $\rightarrow$ `Symbolic Constant` (treated as opaque for now), named by its **full dotted path**, lowercased (`t1.w`). The original reference is stashed in `SymbolicTranslationContext::column_map` and restored by copy in `FromSymbolic`.
    -   `Operator` (+, -, *) $\rightarrow$ `Symbolic Operation`

    A `Symbolic` symbol carries only a name, so naming a data column by `GetColumnName()` alone would drop its qualifier on the way back out — two same-named columns from different tables would collapse into one symbol, and the rebuilt bare reference would be rejected as ambiguous. Keying by full path keeps them distinct; restoring by copy is lossless for multi-part paths (`catalog.schema.table.column`) and for quoted identifiers containing a dot, neither of which survives splitting a dotted string. Lowercasing the path matches DuckDB's case-insensitive identifier resolution, so `T1.W` and `t1.w` stay one symbol.

2.  **Normalization**: The symbolic engine simplifies the expression. This involves:
    -   Expanding parentheses: `2 * (x + 5)` $\rightarrow$ `2x + 10`
    -   Collecting like terms: `x + x` $\rightarrow$ `2x`
    -   Separating constants.

3.  **Symbolic to DuckDB**: The `FromSymbolic` function converts the simplified symbolic expression back into a DuckDB `ParsedExpression`, structured specifically for the Binder.

## 3. Canonical Forms

These are the forms the pipeline works in. **Objectives** are rewritten into theirs here, before the Binder. **Constraints** are not — see §3.1.

### 3.1 Constraints — not this layer

Constraints reach the binder **exactly as the user wrote them**. This layer copies a comparison through untouched; only the wrappers around it (`AND`, `WHEN`, `PER`) are traversed, and only for grammar repair.

The shape below is still the form the pipeline works in:

$$ \sum (c_i \cdot x_i) \leq K - \sum (RowTerm_j) $$

but it is produced by `DecideCanonicalizer` after binding, not here. See `canonicalize.md` for the contract (rules K0–K5) and §4 below for why the split is where it is.

### 3.2 Objectives
Objectives are similarly normalized to:
$$ \text{MAX/MIN } \sum (c_i \cdot x_i) $$
Constant offsets in the objective (e.g., `MAX SUM(x * p + 10)`) are dropped from the optimization problem as they do not affect the optimal choice of $x$, though they are technically preserved in the final projection if needed.

### 3.3 What remains of the normalizer

One path, on objectives only. `SimplifyObjectiveRecursive` recurses through `+`/`-` siblings and through `WHEN` / `PER` wrappers (child 0 only), then for a `SUM` body: translate to `Symbolic`, `expand().simplify()`, keep the terms that carry a decision variable, and rebuild each as `coefficient * decide_part`.

It has one bypass, and only one:

| Path | Body shape | Why SymbolicC++ breaks it | Downstream consumer |
|---|---|---|---|
| Quadratic bypass | `SUM(POWER(linear, 2))`, `SUM((expr)*(expr))` over the same variable | `.expand()` distributes the square and loses the `POWER(linear, 2)` pattern | QP extractor in `physical_decide.cpp::DetectQuadraticPattern` |
| Default | everything else | n/a | Objective extractor |

Detector: `SumInnerIsQuadratic`. A body it does not recognize survives as an opaque leaf, so bilinear (`SUM(x*y)`) and MIN/MAX bodies pass through the default path intact.

**The constraint side had three more bypasses, and they are gone.** A composed MIN/MAX bypass, a quadratic-LHS bypass and a tagged-aggregate rewrite guarded `SimplifyComparisonExpr`, along with a fourth added later for scaled reducers. All four were deleted with the function at C.4. This is not a loss of protection: a bypass exists only to stop *this* layer from damaging structure it does not understand, and the layer no longer touches constraints. `DecideCanonicalizer` needs no equivalent because it never opens a term at all.

**Refactor tripwire**: a *new* bypass here means the objective path is being asked to understand a structure SymbolicC++ cannot represent. The answer is not a fifth guard — it is `canonicalize.md` D4, extending the canonicalizer to objectives, which retires SymbolicC++ entirely.

The architecture comment at the top of `src/decidb/symbolic/decide_symbolic.cpp` is the authoritative reference.

## 4. Relationship to canonicalization

This layer performs **algebraic simplification** — expanding, combining like terms, folding constants. It is partial by nature: opening a `POWER(...)`, a tagged aggregate, or a composed `MIN`/`MAX` would destroy structure the pipeline needs, and declining is the correct behaviour for those shapes.

**Partiality is why it cannot own constraint shape.** A pass that may decline can never be the single home for anything: every consumer downstream has to handle both "it ran" and "it declined", and the declined branch is where duplicate implementations come from. That is exactly what happened — the same left/right migration ended up reimplemented in the physical operator.

**Structural canonicalization is a separate concern with a separate home.** Deciding which side of a comparison each term sits on, and which way the relation points, happens in `DecideCanonicalizer` (`src/planner/decide/decide_canonicalizer.cpp`), after binding, over bound expressions. That pass never looks inside a term — it asks only whether a term is decision-bearing and whether it reduces over rows — which is what lets it be total where this one cannot be.

The distinction matters because the two were historically fused here. Untangling them took six commits (`canonicalize.md` phases A–C, now complete); C.4 deleted the constraint half of this file, and C.2 deleted the last of the five sites — a parsed-level side flip in the constraint binder that rewrote `5 >= x` into `x <= 5` so that everything below could assume a side. The canonicalizer makes that same swap on the bound tree, so the binder now only asks whether *either* side bears a decision. See `07_issues/code_quality/todo.md` for what is still open (Phase D, K3's rejection design, D4/D6).

**A factor on a reducer is not this layer's business either, and used to be.** `CopyAndFoldConstantsIntoAggregates` rewrote `K * AGG(e)` to `AGG(K * e)` here — sound for `SUM`/`AVG`, and a **wrong answer** for `MIN`/`MAX` at a negative `K`, since `MAX(-2x)` is `-2·MIN(x)`. The layer cannot get that right: at the parsed level the aggregate kind is not yet settled (`AVG`→`SUM` and the `MIN`/`MAX` linearizations have not run). There is no fold any more, anywhere: the canonicalizer peels the factor off the reducer and it stays outside all the way to the solver row, which is what makes its sign an optimization input rather than a correctness one.

**A testing gotcha that C.4 reversed.** This layer used to run first on constraints *and* peel numeric offsets, so a shape like `(SUM(x) WHEN c) + 3 <= 9` reached the canonicalizer already migrated and exercised none of it — to reach it with a sealed multi-term spine you needed a term this layer could not peel. That is no longer true: **nothing rewrites a constraint before binding**, so the canonicalizer sees every constraint as written, and any additive shape now exercises it. The inverse hazard is gone too — a working constraint query is now evidence the canonicalizer handled it, because there is no longer a parsed-level path that could have.

The cast asymmetry behind that is still worth knowing. This layer ran before any cast existed, which is why it could walk an additive spine directly. After binding there is no such luxury: `FunctionBinder::CastToFunctionArguments` puts casts between the `+` nodes of a chain, and comparison binding wraps each side whole when the two sides' types differ. `DecideCanonicalizer` therefore descends widening numeric casts and re-applies them per term (`canonicalize.md` §6, B.2).

## 5. Interaction with Binder
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

The condition is intentionally narrow (`c_expr`) so the aggregate-local form does not consume the rest of a DECIDE expression. Comparison predicates for aggregate-local filters therefore need parentheses, for example `SUM(x * w) WHEN (category = 'A')`. Legacy objective syntax such as `MAXIMIZE SUM(x) WHEN category = 'A'` is reassociated in `SimplifyDecideObjective()` back into a whole-objective `WHEN` to preserve existing semantics.

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
ColId '.' ColId '(' variable_type ')'
```

When the parser encounters a dotted name in the `DECIDE` clause (e.g., `DECIDE drivers.assigned(BOOL)`), it produces a qualified `PGColumnRef` with a two-part name list — the first part is the table alias, the second is the variable name. This is the same `PGColumnRef` structure DuckDB uses for qualified column references (e.g., `t.col`), so no new AST node types are required.

During normalization the symbolic layer canonicalizes a decision variable to its **unqualified** name, so `S.keepS` and `keepS` are the same symbol (§2). The table prefix is therefore not carried through algebraic simplification, and does not need to be: `bind_select_node.cpp` registers the unqualified name for every variable and rejects two variables that share one (`Duplicate DECIDE variable name`), so the unqualified form always resolves to the right variable. This is the opposite of the rule for *data* columns, which are keyed by full path precisely because two tables may expose the same column name. The binder (not the parser) is responsible for resolving the table alias and validating that the referenced table exists in the query's bind context.
