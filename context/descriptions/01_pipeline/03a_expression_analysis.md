# Phase 1: Expression Analysis

## Overview

After data materialization (the Sink phase collects all input rows), the first analysis step extracts the structure of constraints and objectives from the bound expression trees. This happens in the `DecideGlobalSinkState` constructor, which calls `AnalyzeConstraint()` and `AnalyzeObjective()` on the bound expressions produced by the binder.

The goal of this phase is purely structural: determine *which* DECIDE variables appear in each constraint/objective and *what* coefficient expressions multiply them. The actual numeric evaluation of those coefficients happens later in Phase 2.

**Key Source File**: `src/execution/operator/decide/physical_decide.cpp` (AnalyzeConstraint/AnalyzeObjective methods in `DecideGlobalSinkState` constructor)
**Header**: `src/include/duckdb/execution/operator/decide/physical_decide.hpp`

## `AnalyzeConstraint()`

Recursive traversal of the bound constraint expression tree. Called once per top-level constraint expression, it descends through wrapper layers and ultimately produces `DecideConstraint` structs.

### Wrapper Detection

Constraints may be wrapped in WHEN and/or PER layers (encoded as tagged `BoundConjunctionExpression` nodes):

1. **PER wrapper**: A `BoundConjunctionExpression` with alias `PER_CONSTRAINT_TAG` and 2+ children: `child[0]` is the constraint (possibly further WHEN-wrapped), `child[1..N]` are the PER column expressions. The method extracts the per_columns and recurses into `child[0]`.

2. **WHEN wrapper**: A `BoundConjunctionExpression` with alias `WHEN_CONSTRAINT_TAG` and 2 children: `child[0]` is the actual constraint, `child[1]` is the WHEN condition. The method extracts the when_condition and recurses into `child[0]`.

3. **AND conjunctions**: Regular `BoundConjunctionExpression` nodes (no special alias) represent multiple constraints joined by AND. Each child is recursively analyzed as an independent constraint.

### Comparison Expression Handling

When a `BoundComparisonExpression` is reached (the actual constraint):

- The comparison type (`<=`, `>=`, `=`, etc.) and RHS expression are recorded directly.
- The LHS is unwrapped through any `BoundCastExpression` layers.
- **Aggregate LHS** (e.g., `SUM(x * cost)` or `SUM(x * cost) WHEN a + SUM(x * hours) WHEN b`): `ExtractAggregateConstraintTerms()` walks additive aggregate expressions, calls `ExtractConstraintTerms()` on each aggregate's child, and copies aggregate metadata onto the extracted terms. The `lhs_is_aggregate` flag is set. Aggregate-local `WHEN` filters are stored on `Term::filter`, bilinear term filters, or quadratic group filters. If the aggregate has alias `AVG_REWRITE_TAG`, the extracted terms are marked for AVG scaling.
- **Per-row LHS**: first the **K1 guard** — `CollectDecideVarRefs()` on the RHS must find nothing. `DecideCanonicalizer` has already moved every decision-bearing term to the left, so a hit here means a rewrite broke canonical form upstream and the constraint is rejected with an internal error rather than mis-solved. Then two sub-paths:
  - **Single-variable** (simple bound like `x <= 5`): `FindDecideVariable()` identifies the variable; coefficient is implicitly 1.
  - **Complex LHS** (e.g. `z_0 + z_1 = 1`, `d - x >= -c` from ABS linearization, or a quadratic `POWER(x - t, 2) <= K`): `ExtractConstraintTerms()` walks the additive spine.

  There used to be a third sub-path that re-partitioned a decision-bearing RHS here, carrying the LHS's data part on `lhs_offset_expr`. It was a second implementation of the canonicalizer's job and was deleted at `canonicalize.md` C.3 after being verified unreachable; the guard above is what replaced it.

## `AnalyzeObjective()`

Extracts terms from the objective's aggregate expression. Handles linear, bilinear, and quadratic objectives — including **mixed** shapes where a quadratic group and linear/bilinear siblings appear inside the same SUM (e.g. `SUM(POWER(x - t, 2) + c * x)`) — and additive objective expressions with aggregate-local filters.

1. Unwraps any `BoundCastExpression` layers.
2. Checks for a WHEN wrapper (same `WHEN_CONSTRAINT_TAG` pattern) and extracts the condition.
3. Expects a `BoundAggregateExpression` (SUM) or an additive expression containing aggregate terms. The SUM argument is walked by `ExtractLinearAndBilinearTerms`, which at **every** additive node probes `PhysicalDecide::DetectQuadraticPattern`:
   - `POWER(linear_expr, 2)` / `POW(linear_expr, 2)` / `(expr) ** 2` — exponent unwrapped from casts (DuckDB wraps the integer literal `2` in a `BoundCastExpression`)
   - `(expr) * (expr)` self-product with identical `ToString()`
   - `-(quadratic_pattern)` (unary negation) and `K * quadratic_pattern` (constant on either side, nested combinations produce composed signs)
   A quadratic match routes the inner linear expression into `squared_terms` with a scalar `quadratic_sign` (carries both negation and constant scaling); linear/bilinear siblings in the same `+`/`-` tree are emitted into `terms` / `bilinear_terms` by the same walker. Pure-linear, pure-quadratic, and mixed objectives therefore go through one unified traversal.
4. **At most one quadratic group per objective**: a second match raises `InvalidInputException` (e.g. `SUM(POWER(x,2)) + SUM(POWER(y,2))` is rejected) because downstream Q construction assumes a single scalar `quadratic_sign`.
5. **Degree guard**: `PhysicalDecide::IsLinearInDecideVars` is invoked on the inner of every POWER/self-product pattern and on each side of a bilinear `*`. Degree > 2 shapes (`POWER(x,2)*POWER(x,2) = x^4`, `POWER(x,2)*POWER(y,2) = x^2 y^2`, `a*POWER(x,2)`, `POWER(POWER(x,2),2)`) are rejected with a clear error instead of silently misclassified as lower-degree Q or bilinear with a garbage coefficient.
6. Stores the result as an `Objective` with `terms` (linear), `squared_terms` (quadratic inner), `bilinear_terms`, `has_quadratic`, `quadratic_sign`, and optional `when_condition` / `per_columns`. Aggregate-local filters are copied onto the terms they came from (linear, bilinear, and quadratic lists each receive the filter independently).

## Variable Bounds Extraction

`TraverseBoundsConstraints()` runs in the `DecideGlobalSinkState` constructor (before `AnalyzeConstraint`) to identify simple per-variable bounds (e.g., `x >= 5`, `x <= 10`, `x BETWEEN 0 AND 100`). These are constraints where:

- The LHS is a bare DECIDE variable (not inside a SUM aggregate)
- The RHS is a constant value

When a bound is absorbed, the source `BOUND_COMPARISON` expression pointer is inserted into `gstate.absorbed_bound_exprs`. `AnalyzeConstraint()` checks that set and skips emitting a `DecideConstraint` for absorbed comparisons — otherwise every such bound would also produce `num_rows` redundant per-row model rows in the linear path. Finalize copies `gstate.absorbed_lower_bounds` / `absorbed_upper_bounds` directly into `solver_input`. The traversal:

- Recurses through AND conjunctions, PER wrappers, and WHEN wrappers (examining only `child[0]` for PER/WHEN); WHEN-guarded comparisons are NOT absorbed because they are conditional per-row
- For comparison expressions: checks that the LHS is not an aggregate, finds the DECIDE variable, extracts the constant RHS value
- Applies the bound: `<=` updates upper_bounds (min), `>=` updates lower_bounds (max), `=` sets both; strict `<` / `>` on integer vars tighten by ±1; strict `<` / `>` on REAL vars are deliberately NOT absorbed so `ApplyComparisonSense` in the model builder can reject them with its clear error message
- Each absorbed bound is also recorded as a `UserBoundSpec {decide_var_idx, sense, k}` in `gstate.user_absorbed_bounds`, so the infeasible diagnosis can re-emit it as a loosenable row (it carries no provenance otherwise — see `08_query_diagnostics/infeasible/done.md`). **A variable's intrinsic domain is excluded:** a `BOOLEAN`-domain variable's `[0,1]` box is never synthesized as a constraint at all (see `03_expressivity/decide/done.md`), so `TraverseBoundsConstraints` only ever sees one here if the user wrote it themselves, redundantly restating the domain; the recording consults `op.is_boolean_var` (threaded from `LogicalDecide`) and skips that restatement (and the default non-negativity for any type) so only genuine user bounds are recorded. The column-bound absorption above still applies to all of them.

## Key Data Structures

### `Term` (defined in `physical_decide.hpp`)

```
struct Term {
    idx_t variable_index;              // Which DECIDE variable (INVALID_INDEX for constants)
    unique_ptr<Expression> coefficient; // Row-varying expression to evaluate later
    int sign = 1;                      // +1 or -1 (used by ExtractTerms for subtraction)
    unique_ptr<Expression> filter;      // Optional aggregate-local WHEN filter
    bool avg_scale = false;             // True when term came from AVG
};
```

A single `coeff_expr * x_i` term. The coefficient is an unevaluated expression tree at this stage -- it will be executed against data chunks in Phase 2. The `sign` field tracks negation from subtraction operators.

### `DecideConstraint` (defined in `physical_decide.hpp`)

```
struct DecideConstraint {
    vector<Term> lhs_terms;              // All additive terms from LHS
    unique_ptr<Expression> rhs_expr;     // RHS expression (may contain aggregates)
    ExpressionType comparison_type;       // COMPARE_LESSTHANOREQUALTO or GREATERTHANOREQUALTO
    bool lhs_is_aggregate = false;        // True if original LHS was an aggregate (e.g., SUM(...))
    idx_t minmax_indicator_idx = DConstants::INVALID_INDEX;  // Indicator var idx for hard MIN/MAX
    string minmax_agg_type;              // "min" or "max" (empty if not minmax)
    idx_t ne_indicator_idx = DConstants::INVALID_INDEX;      // Indicator var idx for not-equal (<>)
    unique_ptr<Expression> when_condition; // Optional WHEN condition (nullptr = unconditional)
    vector<unique_ptr<Expression>> per_columns; // Optional PER grouping columns (empty = no grouping)
    vector<BilinearConstraintTerm> bilinear_terms; // Bilinear aggregate terms
    vector<QuadraticGroup> quadratic_groups; // Quadratic aggregate groups
};
```

### `Objective` (defined in `physical_decide.hpp`)

```
struct Objective {
    vector<Term> terms;                    // Linear objective terms
    unique_ptr<Expression> when_condition; // Optional WHEN condition (nullptr = unconditional)
    vector<unique_ptr<Expression>> per_columns; // Optional PER grouping columns (empty = no grouping)
    vector<Term> squared_terms;            // Inner linear terms for QP: SUM(POWER(expr, 2))
    bool has_quadratic = false;            // True if objective is quadratic
    vector<BilinearTerm> bilinear_terms;   // Bilinear objective terms
};
```

## Helper Functions

All methods on `PhysicalDecide`:

- **`FindDecideVariable(expr)`**: Recursively searches the expression tree for a `BoundColumnRefExpression` whose binding matches any DECIDE variable. Returns the variable index or `INVALID_INDEX`.

- **`ContainsVariable(expr, var_idx)`**: Checks whether the expression tree contains a reference to a specific DECIDE variable. Used by `ExtractCoefficientWithoutVariable`.

- **`ExtractCoefficientWithoutVariable(context, expr, var_idx)`**: Given a multiplication expression containing a DECIDE variable, returns a copy with the variable factor removed. For example, from `x * 5 * l_tax`, removes `x` and returns `5 * l_tax`. If the expression *is* the variable itself, returns constant 1. The surviving factors are re-multiplied through `RebindOperator` — see the note below on why the `ClientContext` is required.

- **`ExtractTerms(context, expr, out_terms)`**: Main visitor for decomposing SUM arguments into terms. Handles:
  - `+` operators: recursively processes all children
  - `-` operators (binary): processes first child, then second child with sign flipped
  - `*` operators: finds the DECIDE variable (if any) and extracts the coefficient
  - Cast expressions: recurses into the child
  - Base case (column ref or constant): either a bare variable (coefficient = 1) or a constant term

- **`ExtractAggregateConstraintTerms(expr, constraint, sign)`** and **`ExtractAggregateObjectiveTerms(expr, objective, sign)`**: Walk additive expressions of aggregate terms. Each `BoundAggregateExpression` must already be rewritten to SUM by the optimizer. These helpers copy aggregate-local `BoundAggregateExpression::filter` into the extracted term metadata and mark terms that came from `AVG_REWRITE_TAG` for Phase 2 scaling.

Static helper functions (not on `PhysicalDecide`):

- **`RebindOperator(context, name, children)`** and its wrapper **`RebindMultiply`**: re-resolve an operator against the children it is actually being given, via `FunctionBinder::BindScalarFunction`. **Every rebuild of a reshaped subtree must go through this.** Hand-assembling a `BoundFunctionExpression` from another node's `function` / `return_type` / `bind_info` does not fail when the children's types disagree — it reinterprets their *physical* representation, returning a plausible wrong number and potentially reading past the end of a narrower vector. DECIDE has hit that failure mode three times; see `07_issues/bugs/done.md` ("A rebuilt product inherited a signature it no longer matched"). Two things guarantee a mismatch after a rewrite: distribution replaces a factor with one of its addends (narrower than the sum it came from), and dropping a factor shifts the survivors out of alignment with `function.arguments`. This is why `ExtractTerms` and `ExtractCoefficientWithoutVariable` take a `ClientContext`; `DecideGlobalSinkState` holds one for the analysis phase, and `Finalize` has its own.

- **`BuildCoefficientFromFactors(context, factors)`**: folds a flattened factor list back into a product, binding each binary node for its own operands. Used by `TryDistributeMultiplyOverAdd` and by bilinear term extraction.

- **`CombineBilinearCoefficients(coef_a, coef_b, mul_func)`**: Combines coefficient expressions extracted from both sides of a bilinear product. Returns no coefficient for `1 * 1`, returns the non-`1` side for one-sided coefficients, and creates a `BoundFunctionExpression("*", ...)` for two non-`1` coefficients. This keeps objective and constraint bilinear extraction consistent for shapes like `(coef_a * x) * (coef_b * y)`.

- **`CollectDecideVarRefs(expr, sign, refs, op)`**: Walks the expression tree tracking sign through `+` and `-` operators, collecting all DECIDE variable references with their accumulated sign (+1 or -1). Its only remaining caller is the K1 guard on the per-row path, which needs the emptiness of the result rather than the signs.
