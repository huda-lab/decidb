# WHEN Keyword — Implemented Features

`WHEN` is a postfix conditional modifier applied to both constraints (in `SUCH THAT`) and objectives (`MAXIMIZE`/`MINIMIZE`). It causes the expression to apply only to rows where the condition is true.

---

**Syntax and basic semantics** (constraint/objective/aggregate-local forms, row filtering vs. coefficient zeroing, parenthesization rules): see `../../00_project_overview/syntax_reference.md` §6. This doc covers the deeper semantics, edge cases, and implementation.

---

## Semantics

### WHEN with Not-Equal (`<>`) Constraints

Both expression-level and aggregate-local `WHEN` compose correctly with `<>` (not-equal) aggregate constraints:

```sql
-- Expression-level WHEN + NE
SUM(x) <> 2 WHEN active

-- Aggregate-local WHEN + NE
SUM(x) WHEN active <> 2
```

The NE Big-M disjunction uses a **single global binary indicator variable** per group (one for WHEN-only, one per group for PER). This is expanded as raw constraints after the `VarIndexer` is built, bypassing the per-row indicator path used by per-row NE constraints. The formulation is:
- `SUM(coeffs) - M*z <= K-1`  (z=0 → SUM ≤ K-1)
- `SUM(coeffs) - M*z >= K+1-M`  (z=1 → SUM ≥ K+1)

Code pointer: deferred aggregate NE expansion in `physical_decide.cpp`, after the single `VarIndexer var_indexer` is built (the same indexer is later threaded through `SolveModel()` and ultimately moved onto `gstate.var_indexer`).

### Aggregate-local WHEN

Aggregate-local `WHEN` attaches to a single aggregate term (independent filters
per term — see spec §6.3 for syntax). An objective condition may contain one
atomic comparison directly, so
`MAXIMIZE SUM(x * profit) WHEN category = 'electronics'` has the same meaning
with or without parentheses. A constraint-local comparison before its bound
still needs parentheses; see the restriction below.

---

## NULL Handling

If the `WHEN` condition evaluates to NULL for a row, that row is treated as **not matching** (same as false).

---

## Empty Row Sets

A `WHEN` filter that matches zero rows on an aggregate (SUM, AVG, MIN, MAX) is **rejected pre-solver** with `InvalidInputException`:

```
DECIDE empty row set for {aggregate|min|max|sum|avg} in {constraint|objective|composed constraint|composed objective}.
An empty aggregate has no well-defined value; check your WHEN clause.
```

**Scope** — rejected cases:
- Constraint- or objective-level `WHEN` that filters every row: `SUM(x*v) <= K WHEN false_condition`.
- Aggregate-local `WHEN` on any single term (SUM/AVG/MIN/MAX) that matches zero rows.
- Composed MIN/MAX term with empty `WHEN`: `SUM(x*v) + (MAX(x*v) WHEN false) <= K`.
- Easy-direction MIN/MAX (`MAX(...) <= K WHEN …`, `MIN(...) >= K WHEN …`): the optimizer strips these to per-row, but a `MINMAX_EASY_REWRITE_TAG` on the rewritten comparison lets the execution layer still enforce the rule.

**Not rejected** (preserved behavior):
- Individual empty groups within `PER`: those groups are skipped downstream. Only rejected when **every** group is empty (the aggregate as a whole sees no rows).
- Per-row constraints with `WHEN` that matches zero rows (e.g., `x <= 0 WHEN never_condition`): this is a valid no-op (the constraint applies to no rows).

**Rationale**: The MIN/MAX reformulation uses a global auxiliary `z` (or per-term `z_k` in composed shapes) that the solver pins via per-row constraints. With zero rows the auxiliary has no per-row pinning and floats free inside its bounds — the outer constraint or objective becomes silently vacuous. Semantically MIN(∅)=+∞ and MAX(∅)=−∞, so a hard-direction bound should be infeasible rather than ignored. Rejecting pre-solver is cleaner than emitting `0 >= 1` and reporting solver infeasibility — the error points directly at the likely `WHEN` typo. SUM and AVG are rejected for consistency (strict "reject all empty sets" rule) even though SUM(∅)=0 and AVG(∅) would only be mathematically undefined in the pure-aggregate case.

**Code pointers**:
- Guard helper: `RejectEmptyAggregate` in `src/execution/operator/decide/physical_decide.cpp`.
- Four insertion sites: constraint `row_group_ids` build (after the WHEN/PER unified evaluation), objective WHEN/per-term filter application, composed MIN/MAX constraint `z_k` loop, composed MIN/MAX objective `z_k` loop.
- Easy-direction tag: `MINMAX_EASY_REWRITE_TAG` in `src/include/duckdb/common/enums/decide.hpp`, set during `RewriteMinMaxInConstraint` in `src/optimizer/decide/decide_optimizer.cpp`, detected at physical plan construction to set `DecideConstraint.was_minmax_easy`.

---

## Rules and Restrictions

Basic rules (conditions reference table columns only — they're evaluated before the solver runs to build the coefficient matrix; compound `AND`/`OR` conditions require parentheses; no-WHEN means unconditional): see spec §6.4. Additional restrictions below.

### Constraint-local comparisons, `NOT`, and arithmetic may need parentheses

The constraint token keeps aggregate-local conditions narrow so it cannot steal
the constraint bound. The objective token admits one atomic comparison because
there is no trailing bound. Conditions containing `NOT`, arithmetic, or compound
logic must be parenthesized on both paths:

```sql
-- Rejected by the parser
SUM(x * v) <= 12 WHEN NOT w
SUM(x * v) <= 12 WHEN a + b > 5
SUM(x * v) WHEN tier = 'high' <= 10

-- Correct
SUM(x * v) WHEN (tier = 'high') <= 10
MAXIMIZE SUM(x * v) WHEN tier = 'high'
SUM(x * v) <= 12 WHEN (NOT w)
SUM(x * v) <= 12 WHEN (a + b > 5)
```

**Actionable parser hint.** The raw bison error for these shapes (`syntax error at or near "NOT"` / `"<="`) is uninformative on its own, so DECIDE queries whose syntax error names one of the WHEN-breaking tokens get a one-line hint appended: *"wrap the WHEN condition in parentheses — e.g. WHEN (a = b), WHEN (NOT flag), or WHEN (a + b > 5)."* The augmentation lives in `src/parser/decide/decide_parse_hints.cpp` (`MaybeAppendDecideWhenHint`), called from `src/parser/parser.cpp` at the syntax-error throw site; it is gated on the query containing `DECIDE` + `WHEN` and the error naming a break token, so it never fires on unrelated syntax errors. Pinned by `test_when_grammar.py::test_when_unparen_error_carries_paren_hint`.

### Expression-level and Aggregate-local WHEN Do Not Mix

DeciDB rejects a constraint or objective that contains both a whole-expression `WHEN` and one or more aggregate-local `WHEN` filters:

```sql
-- ERROR: expression-level WHEN and aggregate-local WHEN in same constraint
(SUM(x * weight) WHEN active + SUM(x * weight) WHEN expedited <= 80) WHEN region = 'US'
```

Move the shared condition into each aggregate-local filter, or keep one expression-level `WHEN`.

### Aggregate-local WHEN Composes with Constraint-LHS Arithmetic

Constant offsets and constant scalar factors on a WHEN-tagged aggregate are supported in constraints. `DecideCanonicalizer` handles them on the bound tree, without touching the WHEN tag:

```sql
-- All OK: WHEN-tagged aggregate composed with constants on the constraint LHS
SUCH THAT SUM(x) WHEN (w > 1) + 3 <= 10                  -- offset peeled to RHS
SUCH THAT 2 * (SUM(x) WHEN active) <= 10                 -- scalar folded into SUM body
SUCH THAT (SUM(x) WHEN active) / 2 <= 5                  -- divisor folded into SUM body
SUCH THAT (SUM(x) WHEN w) + (SUM(y) + 3) <= 10           -- nested parallel sum + offset
```

The outer-WHEN form remains supported and equivalent for single-aggregate constraints:

```sql
SUCH THAT SUM(x) + 3 <= 10 WHEN active
```

**How it works**: `DecideCanonicalizer` (`src/planner/decide/decide_canonicalizer.cpp`) decomposes the bound LHS additively — through `+`, binary `-`, unary `-` and widening casts — and moves every decision-free term to the bound. A WHEN-tagged aggregate is just a term to it: the pass never looks inside one, so the per-aggregate filter cannot be flattened. A factor on the aggregate (`K * (SUM(x) WHEN c)`, `(SUM(x) WHEN c) / K`) is **peeled outward** onto the term rather than folded into its body, and stays outside all the way to the solver row.

Until 2026-08-12 this was done twice — a parsed-level tagged-aggregate path did the same additive peel before binding, and folded the factor *inward*, which is a wrong answer for `MIN`/`MAX` under a negative factor. That path is gone; see `../../01_pipeline/04_canonicalizer/done.md` §3.7.

**Objectives go through the same boundary.** `MAXIMIZE (SUM(x) WHEN cond) + 3`, `MAXIMIZE 2 * (SUM(x) WHEN cond)`, and combinations like `MINIMIZE SUM(x) + SUM(y) WHEN c - 7` are all supported, and are handled by `DecideCanonicalizer::CanonicalizeObjective` on the bound tree rather than at the parsed level. Additive constants don't move `argmax`/`argmin`, so they are peeled out of the body onto `LogicalDecide::objective_constant_offset` — which *accumulates*, so the constant the user wrote survives every later optimizer rewrite.

> **Parser limitation (unchanged)**: writing `SUM(x) WHEN cond + 3 <= K` without parentheses around the condition is a plain `Parser Error` because aggregate-local `WHEN` binds tighter than `>`/`<=` per `POSTFIXOP` precedence, and `%nonassoc` comparisons can't chain. Use `SUM(x) WHEN (cond) + 3 <= K` (parens around the condition).

---

## Note: WHEN vs SQL CASE WHEN

DeciDB's `WHEN` is a **row filter** — it controls whether a constraint or objective *applies* to a row. SQL's `CASE WHEN` is a **value expression** — it produces different values conditionally. These serve different purposes and are not interchangeable.

When you need **conditional coefficients or bounds** (different values per row based on conditions), use a CTE or subquery to pre-compute the value, then reference the resulting column inside DECIDE. This avoids any need to support `CASE WHEN` within the DECIDE clause itself.

### Example 1: Conditional Penalty Weights in Objective

Suppose director hour changes should be penalized 3x and manager changes 2x:

```sql
WITH weighted AS (
  SELECT *,
    CASE WHEN title = 'Director' THEN 3
         WHEN title = 'Manager'  THEN 2
         ELSE 1 END AS penalty_weight
  FROM Employees E JOIN WeeklyPlan P ON E.empID = P.empID
)
SELECT *
FROM weighted
DECIDE new_hours(INT)
SUCH THAT ...
MINIMIZE SUM(penalty_weight * abs(new_hours - hours))
```

`penalty_weight` is a table column by the time DECIDE sees it, so it works as a standard coefficient.

### Example 2: Conditional Effectiveness in Constraint

Suppose director hours count as twice as effective:

```sql
WITH effective AS (
  SELECT *,
    CASE WHEN title = 'Director' THEN 2 ELSE 1 END AS effectiveness
  FROM Employees E JOIN WeeklyPlan P ON E.empID = P.empID
)
SELECT *
FROM effective
DECIDE new_hours(INT)
SUCH THAT
  SUM(new_hours * effectiveness) >= 60 PER projectID
```

Note: this is **not equivalent** to decomposing into separate WHEN constraints (`SUM(new_hours) >= 60 PER projectID AND SUM(new_hours) >= 30 WHEN title='Director' PER projectID`), which creates two independent constraints rather than one combined weighted sum.

### Example 3: Conditional Bounds with Overlapping Conditions

Suppose rent tolerance depends on zipcode and bedroom count, with overlapping conditions:

```sql
WITH toleranced AS (
  SELECT *,
    CASE WHEN zipcode = 10003 THEN 500
         WHEN beds >= 3       THEN 300
         ELSE 150 END AS tolerance
  FROM rentals
)
SELECT *
FROM toleranced
DECIDE syn_rent(INT)
SUCH THAT
  abs(syn_rent - rent) <= tolerance
```

Decomposing into multiple WHEN constraints fails here because the conditions overlap (e.g., a 3-bed apartment in zipcode 10003). SQL `CASE WHEN` evaluates top-to-bottom and returns the first match, giving the correct priority semantics. Pre-computing it as a column preserves that behavior.

### Rejection of inline CASE inside DECIDE

A `CASE` expression placed directly inside a DECIDE constraint or objective is rejected with a friendly user-facing error that points to the supported alternatives (postfix `WHEN`, `PER`, CTE pre-computation).

There are three places that reject one, and they share **one wording**, returned by `DecideCaseUnsupportedMessage()` in `src/planner/expression_binder/decide/decide_binder.cpp`:

| Where the `CASE` sits | Rejected by |
|---|---|
| Inside a reducer argument (`SUM(x * CASE …)`) | `ValidateSumArgumentInternal` |
| Anywhere else in a constraint (`x + CASE … <= 5`) | `DecideConstraintsBinder::BindExpression` |
| Anywhere else in an objective (`SUM(x) + CASE …`) | `DecideObjectiveBinder::BindExpression` |

The last two used to fall through to each binder's generic unsupported-class arm and print `(ExpressionClass::CASE)` — a C++ enum name a SQL user cannot act on, and no mention of the alternatives. A user writing a `CASE` is asking for conditional logic whichever spelling they used, so all three now give the same answer. Pinned by `test/decide/tests/test_error_case_expression.py`, which also asserts `ExpressionClass` never reaches the user.

One shape keeps its own message: a bare `CASE` as the whole left-hand side of a constraint (`(CASE WHEN a>0 THEN x ELSE 0 END) <= 5`) is rejected by the LHS-shape check, which names what a constraint's left side must be. That message is already in SQL terms.

### How DECIDE `WHEN` is tokenized (`WHEN_DECIDE`)

The DECIDE `WHEN` keyword is lexed as a **distinct token `WHEN_DECIDE`**, separate from the `WHEN` used by SQL `CASE … WHEN … THEN`. The scanner filter `base_yylex` (`third_party/libpg_query/src_backend_parser_parser.cpp`) sets an `in_decide_clause` flag when it returns the `DECIDE` token (cleared by the `decide_clause` grammar action) and, while set, rewrites `WHEN` → `WHEN_DECIDE`. The DECIDE grammar productions (`c_expr` aggregate-local atom, `decide_constraint_item`, `decide_objective_item` in `grammar/statements/select.y`) reference `WHEN_DECIDE`, so the DECIDE WHEN never enters the global expression grammar — which previously corrupted ordinary function-call parsing (it collided with `WITHIN GROUP` after a function call). See `../../01_pipeline/01_parser/done.md` §2.

The rewrite is suppressed inside a `CASE … END` (tracked by `decide_case_depth`), so a `CASE` written inside a DECIDE expression still parses with ordinary `WHEN` and is then rejected by the binder with the friendly error above — rather than failing with a raw parser syntax error.

**Lexically nested DECIDE clauses.** A DECIDE subquery inside another DECIDE clause
lexes correctly, to any depth. `in_decide_clause` was once a single `bool`, so an
inner clause cleared it on its way out and a *subsequent outer* `WHEN` lexed as
ordinary SQL `WHEN` and failed to parse — nesting alone worked and an outer `WHEN`
alone worked, only the combination broke. The state is now saved and restored per
clause (`PGDecidePushLexState` / `PGDecidePopLexState`); see
[`../../01_pipeline/01_parser/done.md`](../../01_pipeline/01_parser/done.md) §2.
`test/decide/tests/test_nested_decide.py` pins the combination, both spellings of
the inner clause, and three levels of nesting.

### Summary

| Need | Use |
|------|-----|
| Include/exclude rows from a constraint or objective | `WHEN` (postfix) |
| Different coefficient values per row | `CASE WHEN` in a CTE, referenced as a column |

---

## Interaction with PER

WHEN composes with `PER`. When both are present, `WHEN` filters rows first, then `PER` groups the remaining rows. Each group gets its own constraint.

```sql
-- WHEN filters to 'active' rows, PER groups by department
SUM(x * hours) <= 40 WHEN status = 'active' PER department
```

Expression-level WHEN is a special case of a unified row-grouping system:

| Modifier | `row_group_ids` | `num_groups` |
|----------|-----------------|--------------|
| Neither | empty (fast path) | 0 |
| WHEN only | `0` (included) or `INVALID_INDEX` (excluded) | 1 |
| PER only | `0..K-1` (group assignment) or `INVALID_INDEX` (NULL PER value) | K |
| WHEN + PER | WHEN filters first, PER groups the rest | K (of filtered rows) |

Aggregate-local WHEN is evaluated separately from that row-grouping wrapper. Each extracted aggregate term carries its own optional filter mask. With PER, a row participates in a generated group only when it passes the global expression-level WHEN (if present), has a non-NULL PER key, and contributes to at least one aggregate-local term. Groups that end up empty after WHEN filtering are skipped — no constraint is emitted.

---

## Code Pointers

- **Grammar**: `third_party/libpg_query/grammar/statements/select.y`
  - `decide_objective_item` rule: WHEN (and WHEN+PER) support for objectives
  - `decide_constraint_item` rule: WHEN (and WHEN+PER) support for constraints
  - `func_application WHEN decide_when_condition` in `c_expr`: aggregate-local WHEN support

- **Constraint binder**: `src/planner/expression_binder/decide/decide_constraints_binder.cpp`
  - `BindWhenConstraint()`: Extracts the WHEN condition as a separate boolean expression. Validates that the condition references only table columns, not decision variables.
  - `BindExpression()` dispatch: Recognizes top-level `WHEN_CONSTRAINT_TAG` and calls `BindWhenConstraint`; nested `WHEN_CONSTRAINT_TAG` is aggregate-local and binds through `DecideBinder::BindLocalWhenAggregate`.

- **Objective binder**: `src/planner/expression_binder/decide/decide_objective_binder.cpp`
  - `BindExpression()`: Handles PER stripping on objectives, then WHEN condition extraction on the objective expression. Nested `WHEN_CONSTRAINT_TAG` binds as aggregate-local.

- **Base DECIDE binder**: `src/planner/expression_binder/decide/decide_binder.cpp`
  - `BindLocalWhenAggregate()`: Binds the aggregate child, binds the data-only boolean condition, and stores the condition as `BoundAggregateExpression::filter`.

- **Execution**: `src/execution/operator/decide/physical_decide.cpp`
  - `AnalyzeConstraint()` (`src/optimizer/decide/decide_linear_form.cpp`): Signature takes `when_condition` and `per_columns`. PER tag is unwrapped first (outermost), then WHEN tag is unwrapped inside it.
  - `ExtractAggregateConstraintTerms()` / `ExtractAggregateObjectiveTerms()`: Extract additive aggregate expressions and copy aggregate-local filters onto linear, bilinear, and quadratic terms.
  - `Finalize()`, WHEN+PER unified grouping section: `has_when` / `has_per` flags determine evaluation path.
  - `Finalize()`, WHEN evaluation: WHEN condition evaluated into a `when_mask` boolean vector.
  - `Finalize()`, aggregate-local masks: term-level filters are evaluated and applied before row coefficients are added to the solver input.
  - `Finalize()`, row-group assignment: WHEN-only maps to group `0` or `INVALID_INDEX`; WHEN+PER filters with `when_mask` before PER grouping.

- **Data structures**: `src/include/duckdb/execution/operator/decide/physical_decide.hpp`
  - `DecideConstraint::when_condition`: Optional WHEN condition expression.
  - `DecideConstraint::per_columns`: Optional PER grouping columns (vector).
  - `Objective::per_columns`: Same for objectives.
  - `Term::filter`, `BilinearConstraintTerm::filter`, `DecideConstraint::QuadraticGroup::filter`, and `Objective::BilinearTerm::filter`: Optional aggregate-local WHEN filters carried to coefficient evaluation.

- **Evaluated constraint**: `src/include/duckdb/decidb/solver/solver_input.hpp`
  - `EvaluatedConstraint::row_group_ids`: Per-row group assignment (`INVALID_INDEX` = excluded).
  - `EvaluatedConstraint::num_groups`: `0` = ungrouped fast path, `1` = WHEN-only, `>1` = PER groups.

- **Model builder**: `src/decidb/formulation/ilp_model_builder.cpp`
  - Empty groups are skipped — no constraint is emitted.

- **Tag constants and helpers**: `src/include/duckdb/common/enums/decide.hpp`
  ```cpp
  WHEN_CONSTRAINT_TAG        = "__when_constraint__"
  PER_CONSTRAINT_TAG         = "__per_constraint__"
  IsPerConstraintTag(alias)
  ```
