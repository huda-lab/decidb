# SQL Functions & Expressions — Implemented Features

This file documents which SQL functions and expressions work inside DECIQL clauses today.

---

## Aggregate Functions

### SUM()

The primary aggregate over expressions involving decision variables. Valid in both constraints and objectives.

```sql
MAXIMIZE SUM(x * value)
SUCH THAT SUM(x * weight) <= 50
```

**Code**: Validated in `decide_objective_binder.cpp` and `decide_constraints_binder.cpp` — aggregates other than SUM, AVG, MIN, and MAX are rejected with an error.

### AVG() — Coefficient Scaling at Execution Time

`AVG(expr)` over decision variables is treated as an aggregate constraint like SUM, but terms are scaled by the row count N at execution time so the model represents the average, not the raw sum.

**Semantics**: Standard SQL AVG — divide by count of all rows (decision variables are never NULL). This is always linear since N is a data-determined constant.

**Constraints**: Semantically, `AVG(expr) op K` is equivalent to `SUM(expr) op K*N` where N depends on context:
- No WHEN/PER: N = total row count
- WHEN: N = count of WHEN-matching rows
- PER: N = count of rows in each group
- WHEN+PER: N = count of WHEN-matching rows per group
- Aggregate-local WHEN: N = count of rows matching that aggregate-local filter, within each PER group if PER is present

**Objectives (flat)**: `MAXIMIZE/MINIMIZE AVG(expr)` uses the same optimal assignment as `SUM(expr)` when there is one global denominator. In mixed additive aggregate expressions, PackDB preserves AVG scaling per term so `AVG(a) + SUM(b)` is not treated as `SUM(a) + SUM(b)`.

**Objectives (nested PER)**: `OUTER(AVG(expr)) PER col` is fully supported. Inner AVG scales each row's coefficient by `1/n_g` (group size), producing true per-group averages. Outer AVG maps to SUM (dividing by constant G). See [maximize_minimize/done.md](../maximize_minimize/done.md).

```sql
SUCH THAT AVG(x * weight) <= 10         -- SUM(x*weight) <= 10*N
SUCH THAT AVG(x) <= 0.5                 -- at most half the rows selected (BOOLEAN)
SUCH THAT AVG(x * cost) <= 5 WHEN active -- only among active rows
SUCH THAT AVG(x * cost) WHEN active + SUM(x * fee) WHEN priority <= 100
SUCH THAT AVG(x * hours) <= 8 PER emp   -- per-group average
MAXIMIZE AVG(x * profit)                -- same as MAXIMIZE SUM(x * profit)
```

**Code**: AVG flows through binding natively (no parse-time rewrite), preserving its DOUBLE return type so fractional RHS values survive type coercion. The binders (`decide_constraints_binder.cpp`, `decide_objective_binder.cpp`) accept `"avg"` alongside `"sum"`. The `DecideOptimizer` rewrites AVG to SUM while tagging the aggregate with `AVG_REWRITE_TAG`. At execution time (`physical_decide.cpp`), expression analysis marks extracted terms with `avg_scale`; coefficient evaluation scales linear and bilinear terms by `1/N`, and quadratic inner terms by `1/sqrt(N)`. Exception: for `AVG(expr) <> K` the LHS scaling would produce fractional coefficients and trip the NE integer-step guard, so PackDB sets `EvaluatedConstraint::ne_avg_rhs_scale` and leaves the LHS as SUM; the deferred NE expansion multiplies the RHS by the per-group size instead.

**Tests**: `test/decide/tests/test_avg.py` — 11 test cases covering objectives, constraints, WHEN, PER, WHEN+PER, BOOLEAN, INTEGER, non-linear rejection, `<>` with and without WHEN, and no-decide-var passthrough.

---

### MIN() / MAX() — Per-Row and Big-M Indicator Rewrites

`MIN(expr)` and `MAX(expr)` over decision variables are supported in both SUCH THAT constraints and MAXIMIZE/MINIMIZE objectives. The implementation strategy depends on whether the case is "easy" (naturally per-row) or "hard" (requires Big-M indicator variables and a global auxiliary variable).

#### Easy Constraint Cases (No Big-M)

When the comparison direction already bounds each row individually, no auxiliary variables are needed:

- `MAX(expr) <= K` → per-row constraint: `expr <= K` for every row
- `MIN(expr) >= K` → per-row constraint: `expr >= K` for every row

These are trivially correct: bounding every row satisfies the aggregate bound. PER on easy cases is stripped (redundant, since constraints are already per-row).

#### Hard Constraint Cases (Big-M Indicators)

When the aggregate must be tight (equality or the "wrong" direction), a global auxiliary variable `z` and per-row binary indicators are introduced:

- `MAX(expr) >= K` → global variable `z >= K`, per-row: `z >= expr`, plus Big-M indicators ensuring `z` equals some row's value
- `MIN(expr) <= K` → global variable `z <= K`, per-row: `z <= expr`, plus Big-M indicators
- Equality cases (`MAX(expr) = K`, `MIN(expr) = K`) → both directions constrained

#### Objective Cases

- **Easy objectives**: `MINIMIZE MAX(expr)` and `MAXIMIZE MIN(expr)` — a single global auxiliary variable `z` with per-row linking constraints (`z >= expr_i` for MAX, `z <= expr_i` for MIN). The objective optimizes `z` directly.
- **Hard objectives**: `MAXIMIZE MAX(expr)` and `MINIMIZE MIN(expr)` — requires `z` plus per-row binary indicator variables to ensure `z` equals some row's actual value (Big-M formulation).

#### Composition

- **WHEN**: Composes naturally. WHEN masks filter which rows participate in the MIN/MAX aggregate, and constraint/indicator generation skips non-matching rows.
- **PER (easy cases)**: Stripped as redundant — easy cases already produce per-row constraints.

```sql
-- Easy constraint cases (no Big-M)
SUCH THAT MAX(x * cost) <= 100         -- per-row: x*cost <= 100
SUCH THAT MIN(x * hours) >= 2          -- per-row: x*hours >= 2

-- Hard constraint cases (Big-M indicators)
SUCH THAT MAX(x * cost) >= 50          -- global z, binary indicators
SUCH THAT MIN(x * hours) = 4           -- equality: both directions

-- Objectives
MINIMIZE MAX(x * cost)                 -- easy: global z, minimize
MAXIMIZE MIN(x * profit)               -- easy: global z, maximize
MAXIMIZE MAX(x * profit)               -- hard: z + binary indicators
MINIMIZE MIN(x * cost)                 -- hard: z + binary indicators

-- With WHEN
SUCH THAT MAX(x * cost) <= 50 WHEN category = 'electronics'
MINIMIZE MAX(x * deviation) WHEN active = 1
```

**Code**: The rewrite is performed by `DecideOptimizer::RewriteMinMaxConstraints` and `DecideOptimizer::RewriteMinMaxInConstraint` in `decide_optimizer.cpp`, which detect MIN/MAX aggregates and insert `__MIN__`/`__MAX__` marker tags into the symbolic representation. The binders (`decide_constraints_binder.cpp`, `decide_objective_binder.cpp`) whitelist MIN/MAX alongside SUM and AVG. The symbolic layer (`decide_symbolic.cpp`) recognizes the `__MIN__`/`__MAX__` markers. At execution time, `physical_decide.cpp` generates the appropriate per-row constraints, Big-M indicator constraints, and global auxiliary variables. Global variable and constraint support is provided by `solver_input.hpp` and `ilp_model_builder.cpp`.

---

## Rejected: Non-Linear Scalar Functions over a DECIDE Variable

Any scalar function other than `ABS()` and `POWER(..., 2)` that wraps a decision variable is rejected at bind time with:

```
Binder Error: Scalar function 'sqrt' over a DECIDE variable is not supported:
it would make the model non-linear. Only ABS() and POWER(..., 2) can wrap a
decision variable.
```

This covers (non-exhaustive) `SQRT`, `EXP`, `LN`, `LOG`, `FLOOR`, `CEIL`, `ROUND`, `SIN`, `COS`, `TAN`, and any user-defined or built-in scalar function that doesn't have a dedicated linearization path. The rejection fires in every position: per-row constraint LHS (`SUCH THAT sqrt(x) <= 2`), inside an aggregate (`SUM(exp(x))`, `MAXIMIZE SUM(log(x))`), and nested inside `ABS()` or `POWER()` (e.g., `ABS(sqrt(x) - 1)`).

**Scalar functions wrapping only table columns are not affected** — e.g., `SUM(x * sqrt(l_quantity))` would fold `sqrt(l_quantity)` into a per-row coefficient. The walker only flags a function when one of its arguments transitively contains a DECIDE variable.

**Aggregate mis-uses** (e.g., `BIT_AND(x)`, `STDDEV(x)`) are routed to the existing aggregate-specific rejection in `BindAggregate` with the more informative "only SUM, AVG, MIN, MAX, or COUNT is allowed" message. A catalog lookup distinguishes scalar from aggregate so the two rejection paths don't collide.

**Why this guard exists**: before it, per-row non-linear scalars were silently stripped (`SUCH THAT sqrt(x) <= 2` returned `x = 2` instead of `x = 4`), aggregate non-linear scalars crashed the symbolic layer with `InternalException`, and `ABS(sqrt(x))` FATAL-ed the session at physical execution. The validator catches all three classes at bind time.

**Code**: `ValidateDecideNoNonLinearScalar` in `src/planner/expression_binder/decide_binder.cpp`, called from `src/planner/binder/query_node/bind_select_node.cpp` before `NormalizeDecideConstraints` / `NormalizeDecideObjective` (the pre-pass must run before symbolic normalization because the symbolic layer would otherwise throw on unknown functions).

**Tests**: `test/decide/tests/test_error_binder.py` — `test_nonlinear_scalar_per_row_lhs`, `test_nonlinear_scalar_inside_sum`, `test_nonlinear_scalar_inside_abs` (parametrized over SQRT, EXP, LN, LOG, FLOOR, CEIL, ROUND, SIN, COS).

**POWER exponent check**: The same pre-pass rejects `POWER(base, exp)` / `POW(base, exp)` / `base ** exp` when `base` contains a DECIDE variable and `exp` is not a constant numeric equal to `2`. That covers fractional exponents (`POWER(x, 0.5)`), negative exponents (`POWER(x, -1)`), higher-integer exponents (`POWER(x, 3)`), degenerate exponents (`POWER(x, 0)`, `POWER(x, 1)`), and non-constant exponents (`POWER(x, x)`, `POWER(x, col)`). Previously these tripped `InternalException: FromSymbolic: Non-integer exponents are not supported` during symbolic normalization (which happens before binding), exposing a C++ stack trace. The pre-pass now catches all non-2 cases with the same error messages used by the existing `ValidateQuadraticPower` whitelist inside SUM, so error-text tests stay consistent across SUM and non-SUM contexts.

**Known limitation (pre-existing, out of scope for this rejection)**: `SUM(f(col) * x)` where `f` is an arbitrary scalar function on a data column still trips the symbolic normalizer because `ToSymbolicRecursive` doesn't know those functions. Folding data-only scalar subtrees before normalization is a separate improvement; non-decide-var scalars are accepted by the validator but may fail later if they appear inside a SUM aggregate.

---

## ABS() — Linearized Automatically

`ABS(expr)` over decision variables is automatically linearized using standard ILP techniques. The formulation depends on whether ABS appears in a constraint or in the objective, and on the optimization sense.

### ABS in constraints (two paths: lower-envelope or Big-M)

For each `ABS(expr)` that references a DECIDE variable, the system:

1. Introduces an auxiliary REAL variable `d` (hidden from query output)
2. Adds two lower-bound constraints: `d >= expr` and `d >= -expr`
3. Replaces `ABS(expr)` with `d`

The lower-envelope alone forces `d >= |expr|`. To pin `d` to exactly `|expr|`, one of two mechanisms is used per ABS occurrence:

**Path A (lower-envelope only) — when the constraint context upper-bounds `d`.** Triggered for `ABS(...) <= K` / `ABS(...) < K` (LHS) or `K >= ABS(...)` / `K > ABS(...)` (RHS), and the corresponding aggregate forms `SUM(ABS) <= K`, `MAX(ABS) <= K`, `MIN(ABS) <= K`, `AVG(ABS) <= K`. The constraint itself caps `d` from above; the solver picks `d_i = |e_i|` to minimize slack. No extra variables.

**Path B (Big-M sign-indicator) — for hard-direction shapes.** Triggered for `ABS(...) >= K`, `ABS(...) > K`, `ABS(...) = K`, `ABS(...) <> K`, `ABS(...) BETWEEN a AND b`, ABS in equality / not-equal between aggregates, ABS on both sides of a comparison, and the analogous aggregate forms (`SUM(ABS) >= K`, `MIN(ABS) >= K`, `MAX(ABS) >= K`, etc.). These shapes do not naturally upper-bound `d`, so a binary sign indicator `y ∈ {0,1}` is allocated per ABS term and two Big-M upper-bound constraints are added (same formulation as the MAXIMIZE-objective path below):

- `d <= expr  + 2M·(1−y)`  (active when `y=1`, selecting the positive branch)
- `d <= −expr + 2M·y`      (active when `y=0`, selecting the negative branch)

Combined with the lower envelope, these force `d = |expr|` exactly regardless of constraint direction. `M` is computed at execution time from the bounds of variables in `expr` — **all DECIDE variables referenced inside `ABS(expr)` must have finite bounds** when Path B is used (Path A queries don't need bounds because the constraint itself bounds `d`).

The classifier (`TagAbsConstraintsForBigM` in `decide_optimizer.cpp`) walks the constraint tree once and tags each ABS occurrence with `ABS_NEEDS_BIGM_TAG` if it falls into Path B. WHEN/PER wrappers don't change classification — the per-row Big-M envelope is unconditional, and the WHEN/PER filter only affects which rows participate in the outer aggregate or constraint.

### ABS in MINIMIZE objectives (lower-envelope)

Same lower-envelope rewrite applies. When minimizing `d`, the solver naturally pushes `d` down to `|expr|` — the lower bounds suffice.

### ABS in MAXIMIZE objectives (Big-M upper-bound)

For `MAXIMIZE SUM(ABS(expr))`, the lower-envelope alone is unsound: `d` has no ceiling and the solver pushes it to +∞, producing a spurious "unbounded" error. The fix introduces a binary sign indicator `y ∈ {0,1}` and two Big-M upper-bound constraints that pin `d = |expr|` exactly:

- `d <= expr  + 2M·(1−y)`  (upper-bound when `y=1` selects the positive branch)
- `d <= −expr + 2M·y`      (upper-bound when `y=0` selects the negative branch)

`M` is computed at execution time from the variable bounds involved in `expr`: `M = max_r(|rhs[r]| + Σ |coeff[t][r]| · max(|lb_t|, |ub_t|))` over all non-aux terms. **All DECIDE variables referenced inside `ABS(expr)` must have finite bounds** — if any variable lacks a bound (lb ≤ −1e20 or ub ≥ 1e20), an `InvalidInputException` is raised naming the unbounded variable.

```sql
-- MINIMIZE: lower-envelope, no extra variables needed
MINIMIZE SUM(ABS(new_hours - hours))

-- MAXIMIZE: Big-M upper-bound, binary indicator y allocated per ABS term
MAXIMIZE SUM(ABS(x - target))
-- requires: x <= <upper_bound> (and optionally x >= <lower_bound>)

-- In per-row constraints: bound deviation per row (lower-envelope, always correct)
SUCH THAT ABS(new_qty - l_quantity) <= 5

-- In aggregate constraints: bound total deviation (lower-envelope, always correct)
SUCH THAT SUM(ABS(new_qty - l_quantity)) <= 50
```

`ABS()` without decision variables (e.g., `ABS(col1 - col2)`) is left as regular SQL — no rewrite occurs.

**Code**:
- Constraint classifier: `TagAbsConstraintsForBigM` in `decide_optimizer.cpp` runs before `RewriteAbs`. Walks the constraint tree, classifies each comparison's ABS-bearing sides as Path A (sound) or Path B (Big-M needed), and tags Path-B ABS function expressions with `ABS_NEEDS_BIGM_TAG`. BETWEEN/IN/equality/<> subtrees are conservatively tagged.
- Optimizer rewrite: `DecideOptimizer::RewriteAbs` in `decide_optimizer.cpp`. Phase 1 (`FindAndReplaceAbs`) reads the tag and propagates `needs_bigm` to `AbsPairInfo`. Phase 2 emits the lower envelope unconditionally. For each pair where `needs_bigm || (in_objective && MAXIMIZE)`, also allocates the `y` binary, tags the lower-bound constraints with `ABS_UB_POS_TAG_PREFIX` / `ABS_UB_NEG_TAG_PREFIX`, and pushes an `AbsMaximizeLink{aux_idx, y_idx}` to `LogicalDecide::abs_maximize_links`. The link vector is named `abs_maximize_links` for historical reasons but covers both Big-M users (objective MAXIMIZE and constraint hard-direction).
- Tag constants: `ABS_UB_POS_TAG_PREFIX`, `ABS_UB_NEG_TAG_PREFIX`, `ABS_NEEDS_BIGM_TAG` in `decide.hpp`.
- Execution: `physical_decide.cpp` — tag parsing in `AnalyzeConstraint` sets `DecideConstraint::abs_y_idx`/`abs_is_pos_bound`; these are copied to `EvaluatedConstraint`; the Big-M finalization block (after the bilinear block) iterates `abs_maximize_links`, computes M from variable bounds, and emits two derived `EvaluatedConstraint`s (C_ub1 and C_ub2) per ABS term. The error message at finite-bound check is generic (covers both objective-MAXIMIZE and constraint hard-direction triggers).
- Transfer: `plan_decide.cpp` moves `abs_maximize_links` from logical to physical operator.
- Serialization: `serialize_logical_operator.cpp` fields 230/231 (`abs_maximize_link_aux`, `abs_maximize_link_y`).

**Tests**: `test/decide/tests/test_abs_linearization.py` — 10 test cases covering MINIMIZE objectives, MAXIMIZE objectives (basic + missing-bound error), constraints, WHEN, PER, multiple ABS terms, no-decide-var, and mixed variable types.

---

## Arithmetic Operators

### Multiplication (`*`) — variable x constant or variable x column

```sql
x * 5              -- OK: variable * literal
x * weight         -- OK: variable * column (constant per row)
SUM(x * weight)    -- OK: aggregate of linear product
```

`x * y` (variable times variable) **is supported** as a bilinear term via `DecideOptimizer::RewriteBilinear` (McCormick envelopes when one factor is Boolean; non-convex Gurobi QCQP otherwise). `x * x` / `POWER(x, 2)` is supported as a quadratic (QP) term. See `03_expressivity/bilinear/done.md` and the Quadratic Objectives section of `syntax_reference.md` for solver eligibility.

### Addition / Subtraction (`+`, `-`)

```sql
x + y              -- OK: sum of variables
SUM(x * a + y * b) -- OK: linear combination in aggregate
```

### Division (`/`) by a constant or data column

`x / divisor` is supported in per-row constraints, aggregate constraints, and quadratic objectives (inside `POWER(..., 2)`) as long as the divisor doesn't contain a DECIDE variable. `x / y` between two decision variables is non-linear and rejected at bind time with a clear `Division by a DECIDE variable is not supported` error (previously silently accepted in the per-row path with nonsensical solutions). The divisor folds into the extracted coefficient — for `x / 2`, the solver sees `0.5 * x`; for `x / col`, the per-row coefficient is `1/col[row]`.

```sql
SUCH THAT x / 2 <= 1                       -- OK: equivalent to x <= 2
SUCH THAT SUM(x / weight) <= budget        -- OK: per-row scaled sum
MINIMIZE SUM(POWER(x / 2 - 1, 2))          -- OK: QP with division inside base
MINIMIZE SUM(POWER(x / weight - 1, 2))     -- OK: data-column divisor in QP
```

**Code**:
- Bind-time validation: `IsAllowedNameOverDecideVar` and the dedicated `/`-arm of `ValidateDecideNoNonLinearScalar` (per-row pre-pass) and `ValidateSumArgumentInternal` (SUM/POWER inner) in `src/planner/expression_binder/decide_binder.cpp` reject any `/` whose divisor contains a decide variable.
- Per-row extraction: `ExtractTerms` at `src/execution/operator/decide/physical_decide.cpp` walks `/` by recursing into the numerator and wrapping each emitted coefficient as `coef / divisor`.
- QP linearity check: `IsLinearInDecideVars` in the same file accepts `/` when the divisor is decide-var-free, so quadratic patterns like `POWER(x/2 - 1, 2)` reach the QP extractor.
- Symbolic normalization: `FromSymbolic` in `src/packdb/symbolic/decide_symbolic.cpp` recognises negative-integer Power exponents (which the symbolic library produces for `x / w` as `x * w^-1`) and rebuilds them as `1.0 / base^|k|`. Without this round-trip, `SUM(x / col)` would crash with `Non-integer exponents are not supported in DECIDE normalization`.

### Per-row linear LHS (`+ const`, `- col`, `/ const`, unary `-`)

Per-row constraints accept full linear shapes on the LHS, not just `x op K` and `c*x op K`. The extractor walks the LHS tree to separate decide-variable terms from constants and row-varying data columns; the latter are moved to the RHS per row so the solver sees an algebraically equivalent `sum(decide_terms) op adjusted_rhs` constraint.

```sql
SUCH THAT x + 3 <= 10          -- x <= 7
SUCH THAT x - ps_availqty <= 1 -- per row: x <= 1 + ps_availqty[row]
SUCH THAT -x <= -2             -- x >= 2
SUCH THAT 2 * x + 3 <= 11      -- x <= 4
SUCH THAT x / 2 + 1 <= 3       -- x <= 4
```

**Code**: `ExtractTerms` in `src/execution/operator/decide/physical_decide.cpp` handles `+`, `-` (binary and unary), `*`, `/` (divisor must be decide-var-free), and `CAST`. `ExtractConstraintTerms` delegates there. In `src/packdb/utility/ilp_model_builder.cpp`, the per-row constraint loop subtracts LHS terms whose `variable_index == INVALID_INDEX` (constants / row-data) from the per-row RHS instead of silently dropping them.

**Tests**: `test/decide/tests/test_cons_perrow.py` — `test_perrow_linear_lhs_upper_bound` (parametrized over `x+c`, `x-c`, `x/c`, `c*x+c`, `x/c+c`, `x+c-c`), `test_perrow_unary_minus_lower_bound`, `test_perrow_data_column_in_lhs`, all oracle-verified.

---

## Comparison Operators

### =, <, <=, >, >=

Six standard comparison operators are supported in constraints (including `<>`, documented below).

```sql
SUCH THAT x <= 1
SUCH THAT SUM(x * w) >= 10
SUCH THAT SUM(x) = 5
```

`<>` (not-equal) is supported on both per-row and aggregate constraints. It uses Big-M disjunction with an auxiliary binary indicator variable:

- `SUM(x) <> K` → two constraints: `SUM(x) <= K-1 + M*z` and `SUM(x) >= K+1 - M*(1-z)`
- `x <> K` → same pattern applied per-row

**Complexity**: Adds 1 binary variable and 2 constraints per `<>`. The Big-M value M is computed from variable bounds at execution time. Loose bounds produce weaker LP relaxations.

**Code**: Auxiliary indicator variable created by `DecideOptimizer::RewriteNotEqual` in `decide_optimizer.cpp`; Big-M constraints generated at execution time in `physical_decide.cpp` (`Finalize()`), where data bounds are available.

### BETWEEN ... AND ...

Desugars to `>= lower AND <= upper`. Produces two constraints.

```sql
SUCH THAT SUM(x) BETWEEN 10 AND 50
-- equivalent to: SUM(x) >= 10 AND SUM(x) <= 50
```

### IN (...)

Constrains a value to be in a literal set. Works on both table columns and decision variables.

```sql
SUCH THAT category IN ('A', 'B', 'C')   -- table column (SQL filter)
SUCH THAT x IN (0, 1, 3)                -- decision variable domain restriction
```

**Decision variable IN**: `x IN (v1, ..., vK)` is rewritten at bind time into K auxiliary binary indicator variables with two constraints:
- Cardinality: `z_1 + ... + z_K = 1` (exactly one value selected)
- Linking: `x = v1*z_1 + ... + vK*z_K` (x takes the selected value)

**Complexity**: Adds K binary variables and 2 constraints per IN. For small K (2–5 values) this is cheap. Large K (e.g., 100 values) adds significant model size — consider whether the domain can be expressed as a range constraint instead.

**Optimizations**:
- `x IN (0, 1)` on BOOLEAN → trivially satisfied, no rewrite
- `x IN (v)` single value → rewritten to `x = v`

**Code**: `bind_select_node.cpp` (`RewriteInDomain()`), called before constraint binding. IN on aggregates (e.g., `SUM(x) IN (...)`) remains unsupported.

---

## NULL-Related Expressions

### IS NULL / IS NOT NULL

Supported in WHEN conditions and WHERE clause (not over decision variables).

```sql
imputed_distance = distance WHEN distance IS NOT NULL AND
imputed_distance <= 10 WHEN (distance IS NULL AND mode = 'walk-bike')
```

---

## Boolean / Logical Operators

### AND — Two Roles

1. **Constraint separator** at the top level of `SUCH THAT`
2. **Logical AND** inside a `WHEN` condition (requires parentheses)

### OR

Valid in `WHEN` conditions and `WHERE` only. Not supported as a constraint combiner over decision variables (would create non-linear disjunctions).

---

## Summary Table (Implemented Only)

| Function / Operator | In Constraints | In Objective | In WHEN / WHERE |
|---|---|---|---|
| `SUM()` over dec. vars | Yes | Yes | N/A |
| `AVG()` over dec. vars | Yes (RHS scaled) | Yes (→SUM) | N/A |
| `MIN()` / `MAX()` over dec. vars | Yes (per-row / Big-M) | Yes (global aux / Big-M) | N/A |
| `ABS()` over dec. vars | Yes (linearized) | Yes (linearized) | N/A |
| `*` (var x const/col) | Yes | Yes | N/A |
| `*` (var x var, bilinear) | Yes (McCormick / Gurobi QCQP) | Yes (McCormick / Gurobi non-convex) | N/A |
| `POWER(expr, 2)` / `expr ** 2` (QP) | N/A | Yes (convex: both solvers; non-convex: Gurobi) | N/A |
| `+`, `-` (binary and unary) | Yes (per-row linear LHS shapes fully supported) | Yes | Yes |
| `/` (by data column or constant) | Yes | Yes | N/A |
| `=`, `<`, `<=`, `>`, `>=` | Yes | N/A | Yes |
| `<>` (not-equal) | Yes (Big-M) | N/A | Yes |
| `BETWEEN` | Yes | N/A | Yes |
| `IN (...)` | Yes (dec. vars + columns) | N/A | Yes |
| `IS NULL` / `IS NOT NULL` | N/A | N/A | Yes |
| `AND` (constraint sep.) | Yes | N/A | N/A |
| `AND` / `OR` (logical) | N/A | N/A | Yes |
| Any other scalar (`SQRT`, `EXP`, `LN`, `LOG`, `FLOOR`, `CEIL`, `ROUND`, trig, ...) over a DECIDE variable | **Rejected** (non-linear) | **Rejected** (non-linear) | Yes (over data columns) |
