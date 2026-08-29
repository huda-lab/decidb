# Problem Types — Implemented Features

DeciDB can express several classes of mathematical optimization problems. The problem class is determined automatically by the combination of variable types declared in `DECIDE` and the form of the objective expression. This page catalogs the supported problem classes from a mathematical optimization perspective — for syntax details, see the per-keyword done.md files.

---

## Supported Problem Classes

### LP (Linear Programming)

All variables declared `REAL`, with a linear objective and linear constraints.

Model form: minimize c^T x, subject to Ax <= b and l <= x <= u. DeciDB
defaults l to 0, but an explicit finite negative lower bound makes a variable
signed.

```sql
SELECT id, ROUND(x, 2) FROM data
DECIDE x(REAL)
SUCH THAT x <= 100 AND SUM(x) >= 500
MINIMIZE SUM(x * cost)
```

Supported by both Gurobi and HiGHS.

### ILP (Integer Linear Programming)

All variables declared `INT` and/or `BOOL`, with a linear objective and linear constraints. This is the **default** problem class.

Model form: minimize c^T x, subject to Ax <= b, x in Z^n, and l <= x <= u.
INT variables default l to 0, but support explicit finite negative lower
bounds; BOOL variables remain in [0, 1].

```sql
SELECT * FROM items
DECIDE keep(BOOL)
SUCH THAT SUM(keep * weight) <= 50
MAXIMIZE SUM(keep * value)
```

Supported by both Gurobi and HiGHS.

### MILP (Mixed-Integer Linear Programming)

Mix of `REAL` + `INT`/`BOOL` variables, with a linear objective and linear constraints. Arises when a query needs both selection (BOOL) and value-assignment (REAL).

```sql
SELECT * FROM employees
DECIDE selected(BOOL), new_salary(REAL)
SUCH THAT selected <= 1 AND new_salary <= max_salary AND SUM(new_salary) <= budget
MAXIMIZE SUM(selected * performance)
```

Supported by both Gurobi and HiGHS.

### QP (Quadratic Programming)

All variables declared `REAL` (continuous), with a quadratic objective and linear constraints.

Model form: minimize (1/2) x^T Q x + c^T x, subject to Ax <= b and
l <= x <= u. DeciDB defaults l to 0, but supports explicit finite negative
lower bounds.

**Convex QP** (MINIMIZE with PSD Q, or MAXIMIZE with NSD Q): Supported by both Gurobi and HiGHS.

```sql
-- MINIMIZE convex: Q is PSD (standard)
SELECT id, ROUND(x, 2) FROM data
DECIDE x(REAL)
SUCH THAT x >= 0 AND x <= 100
MINIMIZE SUM(POWER(x - target, 2))

-- MAXIMIZE concave (negated quadratic): Q is NSD, both solvers handle natively
SELECT id, ROUND(x, 2) FROM data
DECIDE x(REAL)
SUCH THAT x >= 0 AND x <= 100
MAXIMIZE SUM(-POWER(x - target, 2))
```

**Non-convex QP** (MAXIMIZE with PSD Q): **Gurobi only** — requires spatial branching (NonConvex=2). HiGHS rejects with an error. NP-hard even for continuous variables.

```sql
-- MAXIMIZE convex: push x to boundary (maximum squared deviation)
SELECT id, ROUND(x, 2) FROM data
DECIDE x(REAL)
SUCH THAT x >= 0 AND x <= 10
MAXIMIZE SUM(POWER(x - 5, 2))
```

### MIQP (Mixed-Integer Quadratic Programming)

Mix of `REAL` + `INT`/`BOOL` variables, with a quadratic objective and linear constraints. Arises when combining selection (BOOL) with least-squares fitting (REAL).

**Gurobi only** — HiGHS declares `miqp` unsupported, so the query is refused at plan time with a message naming Gurobi and offering `x(REAL)` as the alternative.

```sql
SELECT * FROM measurements
DECIDE keep(BOOL), repaired(REAL)
SUCH THAT repaired >= 0 AND repaired <= 100 AND SUM(keep) >= 10
MINIMIZE SUM(POWER(repaired - measured, 2))
```

### Mixed Linear + Quadratic Objective

Linear terms may appear alongside a `POWER(...)` term in the same objective, either within a single `SUM` or across sibling `SUM`s. The quadratic part populates the Q matrix; the linear part populates the c vector. Both are emitted from the objective extraction layer simultaneously and composed by the solver.

```sql
-- Linear regularisation on top of least-squares
SELECT * FROM data
DECIDE x(REAL)
SUCH THAT x >= 0 AND x <= 100
MINIMIZE SUM(POWER(x - target, 2) + penalty * x)

-- Equivalent form (sibling SUMs) — produces the same solver input
MINIMIZE SUM(POWER(x - target, 2)) + SUM(penalty * x)
```

Optimum of `SUM(POWER(x - t, 2) + c * x)` with no other binding constraints is `x = t - c/2` (clipped to bounds).

**Restriction — single quadratic group per objective.** Only one `POWER(...)` / `(expr)*(expr)` self-product may appear in an objective (plus arbitrarily many linear siblings). Multiple quadratic groups (e.g., `SUM(POWER(x, 2)) + SUM(POWER(y, 2))`) are rejected with a clear error because the objective-side Q matrix is built from a single inner linear expression. Constraints don't have this restriction — they carry `quadratic_groups` per group.

### Bilinear Programming (Boolean × Anything — McCormick Linearization)

When one factor in a product of two different DECIDE variables is declared `BOOL`, the product `b * x` is exactly linearized using McCormick envelopes. This produces an equivalent MILP reformulation — no relaxation, exact for binary variables. Works with both Gurobi and HiGHS.

**Requires**: A finite upper bound on the non-Boolean variable — explicit (`x <= K`) or inferred via implied-bound propagation from a non-negative constraint like `SUM(x) <= K` (see `../../01_pipeline/05_optimizer/done.md`). Bool×Bool uses simpler AND-linearization (no Big-M needed).

```sql
-- Boolean x Real objective (both solvers)
SELECT * FROM items
DECIDE b(BOOL), x(REAL)
SUCH THAT x <= 100 AND SUM(b) <= 5
MAXIMIZE SUM(b * x)

-- Boolean x Boolean objective (both solvers)
SELECT * FROM tasks
DECIDE b1(BOOL), b2(BOOL)
SUCH THAT SUM(b1) <= 3 AND SUM(b2) <= 3
MAXIMIZE SUM(b1 * b2)

-- With data coefficient
MAXIMIZE SUM(profit * b * x)
```

### Non-Convex Bilinear Programming (Q Matrix, Gurobi Only)

When neither factor is Boolean (`Real×Real`, `Int×Int`, `Int×Real`), the product produces off-diagonal entries in the Q matrix. This is always indefinite (non-convex).

- **Objectives**: Gurobi only (via `NonConvex=2`). HiGHS rejects with a clear error.
- **Constraints**: Gurobi only (via `GRBaddqconstr`). HiGHS rejects with a clear error.

```sql
-- Real x Real objective (Gurobi only)
SELECT * FROM data
DECIDE x(REAL), y(REAL)
SUCH THAT x <= 10 AND y <= 10
MINIMIZE SUM(x * y)

-- Bilinear in constraints (Gurobi only)
SUCH THAT SUM(x * y) <= 100
```

### Feasibility Problems (No Objective)

Any combination of variable types, with constraints but **no** `MAXIMIZE`/`MINIMIZE` clause. The solver finds any feasible assignment satisfying all constraints. The `DecideSense::FEASIBILITY` enum value is used internally; the model builder sets all objective coefficients to zero.

```sql
SELECT * FROM shifts
DECIDE assigned(BOOL)
SUCH THAT SUM(assigned) >= 3 PER day AND SUM(assigned) <= 5 PER employee
```

Supported by both Gurobi and HiGHS.

### QCQP (Quadratically Constrained Quadratic Programming)

Quadratic terms (`POWER(linear_expr, 2)`) are supported in `SUCH THAT` constraints, enabling QCQP. The constraint takes the form `SUM(POWER(expr, 2)) <= K` or per-row `POWER(expr, 2) <= K`.

**Gurobi only** — HiGHS declares `quadratic_constraints` unsupported, so the query is refused at plan time.

```sql
-- Aggregate quadratic constraint: total squared deviation budget
SELECT id, ROUND(x, 2) FROM data
DECIDE x(REAL)
SUCH THAT x >= 0 AND x <= 100
    AND SUM(POWER(x - target, 2)) <= 1000
MAXIMIZE SUM(x)

-- Per-row quadratic constraint
SUCH THAT POWER(x - target, 2) <= 9

-- QCQP: quadratic in both objective and constraint
MINIMIZE SUM(POWER(x - preferred, 2))
SUCH THAT SUM(POWER(x - required, 2)) <= 50
```

All syntax forms supported: `POWER(expr, 2)`, `expr ** 2`, `(expr) * (expr)` self-product. Negated and scaled forms also supported: `-POWER(expr, 2)`, `K * POWER(expr, 2)`.

Composes with WHEN, PER, and linear constraints. Multiple quadratic constraints per query are supported.

---

## How Problem Class Is Determined

The user does not declare a problem class. DeciDB infers it from the variable types and objective form:

| Variable Types | Objective | Problem Class |
|---|---|---|
| All BOOL/INT | Linear or none | ILP |
| All REAL | Linear or none | LP |
| Mix of REAL + INT/BOOL | Linear or none | MILP |
| All REAL | Convex quadratic | QP |
| All REAL | Non-convex quadratic (`MAXIMIZE SUM(POWER)`) | Non-convex QP (Gurobi only) |
| Mix of REAL + INT/BOOL | Quadratic | MIQP (Gurobi only) |
| Any (one factor BOOL) | Bilinear (`b * x`) | MILP (McCormick, both solvers) |
| Any (no BOOL factor) | Bilinear (`x * y`) | Non-convex QP (Gurobi only) |
| Any | Linear/quadratic, with `POWER(expr,2)` constraints | QCQP (Gurobi only) |
| Any | None (feasibility) | Feasibility |

The model builder sets `is_integer`, `is_binary`, `has_quadratic_objective`, and `nonconvex_quadratic` flags accordingly, and the solver backend handles the appropriate formulation.

---

## Solver Support Matrix

This table is `SolverModelClass`, read as a matrix. "Refused
at plan time" means the query is rejected before it reads a row, with a message naming
Gurobi as the solver to install — see `01_pipeline/05_optimizer/done.md` §0.

| Problem Class | Gurobi | HiGHS |
|---|---|---|
| LP | Yes | Yes |
| ILP | Yes | Yes |
| MILP | Yes | Yes |
| QP (convex) | Yes | Yes |
| QP (non-convex) | Yes (NonConvex=2) | **No** — refused at plan time |
| MIQP | Yes | **No** — refused at plan time |
| Bilinear (Bool × anything) | Yes (McCormick → MILP) | Yes (McCormick → MILP) |
| Bilinear (non-convex) | Yes (Q matrix, NonConvex=2) | **No** — refused at plan time |
| Bilinear constraints | Yes (`GRBaddqconstr`) | **No** — refused at plan time |
| QCQP (quadratic constraints) | Yes (`GRBaddqconstr`) | **No** — refused at plan time |
| Feasibility | Yes | Yes |

---

## Key Structural Properties

### Constraints

Constraints are primarily linear. Products of two decision variables (`x * y`) and quadratic terms (`POWER(expr, 2)`) are also supported:

- **Boolean × anything**: Exactly linearized via McCormick envelopes (both solvers). The bilinear product is replaced with auxiliary variables and linear constraints at optimizer time, so the feasible region remains a convex polytope.
- **General non-convex bilinear** (`Real×Real`, `Int×Int`, `Int×Real`): Gurobi only, via `GRBaddqconstr`. On HiGHS the query is refused at plan time.
- **Quadratic constraints** (`POWER(expr, 2)` in SUCH THAT): Gurobi only, via `GRBaddqconstr`. Each `POWER(inner_expr, 2)` is expanded into Q = sign * A^T A (outer product of inner expression coefficients). Multiple quadratic groups per constraint are accumulated. Constant terms from the inner expression are moved to the RHS. Composes with WHEN, PER, and linear terms in the same constraint.

### Quadratic Objectives — Convexity and Sign

Supported quadratic forms:

- `MINIMIZE SUM(POWER(linear_expr, 2))` — convex QP (Q = A^T A, PSD). Both solvers.
- `MAXIMIZE SUM(-POWER(linear_expr, 2))` — concave MAXIMIZE (Q = -A^T A, NSD). Both solvers handle natively (HiGHS internally flips sign+sense).
- `MAXIMIZE SUM(POWER(linear_expr, 2))` — non-convex (Gurobi only, via NonConvex=2). NP-hard.

The following are rejected:

- `MINIMIZE SUM(POWER(x, 3))` — only exponent 2 is supported
- **Total degree > 2** in decision variables — the detector rejects products whose combined degree exceeds 2, even when each factor individually matches a supported pattern. Concretely:
  - `SUM(POWER(x, 2) * POWER(x, 2))` (= x⁴): self-product of a squared expression. Rejected with "self-product of a non-linear expression".
  - `SUM(POWER(x, 2) * POWER(y, 2))` (= x²y²): product of two quadratics. Rejected with "degree > 2".
  - `SUM(a * POWER(x, 2))` where `a` is a DECIDE variable (= a·x²): variable multiplied by a quadratic. Rejected with "degree > 2".
  - `SUM(POWER(POWER(x, 2), 2))` (= x⁴): POWER of a non-linear inner. Rejected with "non-linear expression inside POWER".
  - Same rejections apply inside `SUCH THAT` (quadratic constraints).

### Bilinear Objectives

Products of two different DECIDE variables in objectives:

- **Boolean × anything** (`SUM(b * x)`): McCormick linearization produces an equivalent MILP. Both solvers. Requires a finite upper bound on the non-Boolean variable (explicit or inferred via implied-bound propagation). Bool×Bool uses simpler AND-linearization (no Big-M).
- **General non-convex** (`SUM(x * y)` where neither is Boolean): Off-diagonal Q matrix entries (always indefinite). Gurobi only (NonConvex=2). On HiGHS the query is refused at plan time.
- **Mixed objectives**: Linear + bilinear (`SUM(cost + b*x)`), bilinear + quadratic (`SUM(POWER(x-t,2) + b*x)`), and data coefficients (`SUM(profit * b * x)`) are all supported.

**Syntax forms** (all equivalent for positive quadratic):

```sql
MINIMIZE SUM(POWER(x - target, 2))         -- POWER function
MINIMIZE SUM((x - target) ** 2)             -- ** operator
MINIMIZE SUM((x - target) * (x - target))   -- explicit self-multiplication
```

**Negated syntax forms** (all equivalent for negative quadratic):

```sql
MAXIMIZE SUM(-POWER(x - target, 2))        -- negation outside POWER
MAXIMIZE SUM(-1 * POWER(x - target, 2))    -- explicit -1 multiplication
```

**Mathematical formulation**: The objective `SUM(sign * POWER(a1*x + a2*y + c, 2))` is expanded into the standard QP form `(1/2) x^T Q x + c^T x` where:
- Q is built from the outer product of the inner linear expression's coefficients: Q[i,j] = sign * 2*a_i*a_j summed over rows (factor of 2 due to the (1/2) x^T Q x convention)
- Linear terms arise from constant parts of the inner expression (cross terms: sign * 2*c*a_i)
- sign = +1.0 for positive quadratic, sign = -1.0 for negated quadratic
- The Q matrix is stored in COO (Coordinate) format in `SolverModel`, then converted to CSC for HiGHS

### Variable Bounds and Signed Domains

Variable types have these default bounds:

- BOOL: [0, 1]
- INT: [0, 1e30]
- REAL: [0, 1e30]

For INT and REAL, 0 is a default rather than an intrinsic floor. An
explicit finite negative lower bound such as `x >= -10`, `x BETWEEN -10 AND
10`, or a negative literal in an `IN` domain makes the variable signed. Bound
absorption stores the resulting domain directly as solver column bounds.

A fully-free `(-inf, +inf)` domain is not supported; signed variables must have
a finite lower bound. See [decide/done.md](../decide/done.md) for the detailed
semantics and limitations.

### Big-M Linearization Is Transparent

Several constructs (`<>`, `IN` on decision variables, hard `MIN`/`MAX` cases) use Big-M linearization internally. From the problem classification perspective, these remain linear — they add auxiliary binary variables and constraints but do not change the user-visible problem class (though an LP with Big-M becomes a MILP internally due to the auxiliary binaries).

---

## Cross-References

- Variable type declarations: [decide/done.md](../decide/done.md)
- Objective forms (linear): [maximize_minimize/done.md](../maximize_minimize/done.md)
- Constraint forms: [such_that/done.md](../such_that/done.md)
- SQL functions and linearization: [sql_functions/done.md](../sql_functions/done.md)
- Bilinear terms (`x * y`): [bilinear/done.md](../bilinear/done.md)
- Solver backends and dispatch: [01_pipeline/07_solver/done.md](../../01_pipeline/07_solver/done.md)
- Model building (variable type -> solver flags): [01_pipeline/06_model_formulation/done.md](../../01_pipeline/06_model_formulation/done.md)

---

## Code Pointers

- **Variable type -> solver flags**: `src/decidb/formulation/ilp_model_builder.cpp`
  - DOUBLE/FLOAT -> `is_integer=false`, BOOLEAN -> `is_binary=true`, INTEGER -> `is_integer=true`
  - These flags determine whether the solver treats the problem as LP, ILP, or MILP

- **Quadratic objective detection**: `src/execution/operator/decide/physical_decide.cpp`
  - `DetectQuadraticPattern` (in `src/optimizer/decide/decide_linear_form.cpp`, invoked from `ExtractLinearAndBilinearTerms` at every additive node) matches `POWER(expr, 2)`, `(expr) * (expr)` self-product, and negated / constant-scaled forms. Returns `{inner_linear_expr, sign}`.
  - Extracts inner linear expression terms into `Objective::squared_terms` with `quadratic_sign` (scalar; sign combines negation and constant scaling).
  - **Degree guard**: `IsLinearInDecideVars` (same file) is invoked on the inner of every POWER / self-product pattern and on each side of a bilinear `*`. Inputs whose total decision-variable degree would exceed 2 (e.g. `POWER(x,2)*POWER(x,2)`, `POWER(x,2)*POWER(y,2)`, `a*POWER(x,2)`, `POWER(POWER(x,2),2)`) are rejected with a clear `InvalidInputException` rather than silently misclassified as a lower-degree Q term. Same guard runs in the constraint path (`TryDetectConstraintQuadratic` and the constraint bilinear branch).

- **Q matrix construction**: `src/decidb/formulation/ilp_model_builder.cpp`
  - Builds Q from outer products of per-row inner expression coefficients: Q = sign * A^T A
  - sign = +1.0 produces PSD Q (convex), sign = -1.0 produces NSD Q (concave)
  - Sets `nonconvex_quadratic` flag based on sign+sense combination
  - Handles constant-term cross-contributions to linear objective (also sign-adjusted)

- **Gurobi QP**: `src/decidb/gurobi/gurobi_solver.cpp` — calls `GRBaddqpterms` for Q matrix; sets `NonConvex=2` when `nonconvex_quadratic` is true

- **HiGHS QP**: `src/decidb/naive/deterministic_naive.cpp` — calls `passHessian` with COO->CSC conversion. It contains no model-class check: non-convex QP, MIQP and quadratic constraints are all declared unsupported in its capability table and refused at plan time (`src/optimizer/decide/decide_solver_gate.cpp`)

- **Solver dispatch**: `src/decidb/solver/ilp_solver.cpp`
  - `SolverModel::Build()` constructs the formulation; `SolveModel()` dispatches to Gurobi (if available) or HiGHS

- **Solver input (Q matrix storage)**: `src/include/duckdb/decidb/formulation/solver_input.hpp`
  - `quadratic_inner_coefficients`, `quadratic_inner_variable_indices`, `has_quadratic_objective`

- **SUM argument validation (QP + bilinear syntax)**: `src/planner/expression_binder/decide/decide_binder.cpp`
  - `ValidateSumArgumentInternal` accepts `POWER(linear_expr, 2)`, `POW(linear_expr, 2)`, `(expr) * (expr)` where both sides are identical, and products of two different DECIDE variables (`x * y`) when `allow_bilinear` is true
  - Rejects `POWER(expr, N)` for N != 2, triple or higher products, non-constant exponents

- **Quadratic constraint extraction**: `src/execution/operator/decide/physical_decide.cpp`
  - `ExtractConstraintTerms` detects `POWER(expr, 2)`, `expr ** 2`, `(expr)*(expr)` self-products, and scaled forms
  - `TryDetectConstraintQuadratic` (local lambda) handles pattern matching for all syntax forms
  - Populates `DecideConstraint::QuadraticGroup` with inner linear terms and sign

- **Quadratic constraint Q matrix**: `src/decidb/formulation/ilp_model_builder.cpp`
  - `BuildQuadraticConstraint` lambda builds `QuadraticConstraint` from `EvaluatedConstraint` quadratic groups
  - Computes outer product Q = sign * A^T A per group, accumulates into single Q matrix
  - Handles PER groups (one QuadraticConstraint per group), WHEN filtering, and per-row constraints

- **Feasibility support**: Grammar rule in `third_party/libpg_query/grammar/statements/select.y` accepts `DECIDE ... SUCH THAT ...` without objective. `DecideSense::FEASIBILITY` flows through parser → binder → physical → model builder. Model builder sets all objective coefficients to zero.

- **No pre-binding expansion**: nothing expands a constraint or an objective before
  binding, so the `POWER(linear, 2)` pattern the QP extractor matches on always survives
  intact. `DecideCanonicalizer` never opens a term either, so no skip or bypass is needed
  anywhere for quadratic shapes.

- **Bilinear implementation**: See [bilinear/done.md](../bilinear/done.md) for full implementation details including:
  - McCormick rewrite pass in optimizer (`RewriteBilinear`)
  - Boolean type tracking (`is_boolean_var` vector)
  - McCormick Big-M generation at execution time
  - Q matrix off-diagonal entries for non-Boolean bilinear
  - Quadratic constraints via `GRBaddqconstr`
