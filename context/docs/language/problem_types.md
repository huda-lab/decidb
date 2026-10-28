# Problem types

The user never names a problem class. DeciDB infers it from the declared types and the shape of the terms, and
refuses at plan time, before reading a row, any class the active solver cannot handle.

## Classes

| Variables | Objective / constraints | Class | Gurobi | HiGHS |
|---|---|---|---|---|
| all `REAL` | linear | LP | yes | yes |
| all `INT`/`BOOL` | linear | ILP | yes | yes |
| mixed | linear | MILP | yes | yes |
| all `REAL` | convex quadratic objective | QP | yes | only if Q is non-singular |
| any | `MAXIMIZE` a sum of squares | non-convex QP | yes (`NonConvex=2`) | refused |
| mixed | quadratic objective | MIQP | yes | refused |
| any, one factor `BOOL` | `b * x` | MILP (McCormick) | yes | yes |
| any, no `BOOL` factor | `x * y` | non-convex QP | yes | refused |
| any | `POWER(e, 2)` or `x * y` in `SUCH THAT` | QCQP | yes (`GRBaddqconstr`) | refused |
| any | no objective | feasibility | yes | yes |

The table is `SolverModelClass` (`src/include/duckdb/common/decide_solver_capabilities.hpp`) read as a matrix. A
refusal comes from `RequireDecideSolverSupport` (`decide_solver_gate.cpp`) and names what the query does and which
solver to install. It never blames the query, because the same SQL runs on a host with Gurobi.

HiGHS refuses a coupled or rank-deficient Q because it can stop on a flat valley and report a wrong optimum.

Big-M rewrites (`<>`, `IN`, `ABS` Path B, hard `MIN`/`MAX`, `norm(e, 0)`) add binaries, so an LP that uses one is a
MILP internally. The user-visible class does not change.

## Domains

| Type | Default box |
|---|---|
| `BOOL` | `[0, 1]` |
| `INT` | `[0, ∞)` |
| `REAL` | `[0, ∞)` |

Zero is a default, not a floor: a finite negative lower bound (`x >= -10`, `BETWEEN -10 AND 10`, a negative value in
`IN`) makes `INT` or `REAL` signed ([functions.md](functions.md#declarations)). A fully free variable is not
supported; a signed variable needs a finite lower bound.

## Quadratic terms

- **Spellings.** `POWER(e, 2)`, `e ** 2` and `(e) * (e)` are the same square. A minus sign or a constant factor in
  front scales it (`-POWER(e, 2)`, `K * POWER(e, 2)`). The inner `e` is linear in the decisions.
- **Objective.** `sign · Σ (a·x + c)²` becomes `½ xᵀQx + cᵀx` with `Q[i,j] = sign · 2·aᵢ·aⱼ`, summed over rows, and
  the cross terms `sign · 2·c·aᵢ` going to the linear part. `MINIMIZE` a sum of squares is convex; `MAXIMIZE` its
  negation is concave and both solvers take it; `MAXIMIZE` a sum of squares is non-convex. Q is kept in COO form and
  converted to CSC for HiGHS (`passHessian`).
- **One quadratic group per objective.** The objective's Q is built from a single inner linear expression, so
  `SUM(POWER(x, 2)) + SUM(POWER(y, 2))` is refused. Any number of linear siblings may sit beside the one group.
  Constraints have no such limit: each constraint carries its own `quadratic_groups`, accumulated into one Q.
- **Constraints.** `SUM(POWER(e, 2)) <= K` or per row `POWER(e, 2) <= K`. Constants of the inner move to the bound.
  Quadratic constraints compose with scopes and with linear terms in the same clause.
- **Under MIN or MAX.** A square or product inside a `MIN`/`MAX` (`MAX(POWER(x - t, 2)) >= K`,
  `MINIMIZE MAX(POWER(x - t, 2))`), a squared `SUM` beside one (`SUM(POWER(x, 2)) + MAX(x) <= K`), or the inner
  reducer of a two-level objective under one (`MAX(per g: SUM(POWER(x - t, 2)) by (g))`): each member becomes a
  helper decision per row with one quadratic row, and the extremum is stated to the solver natively, so it is Gurobi
  only; HiGHS refuses, naming the reducer. `MAX(POWER(e, 2)) <= K` and `MIN(POWER(e, 2)) >= K` need no helper: they
  are one quadratic row per member.
- **Degree guard.** The total decision degree of a term is at most 2. `IsLinearInDecideVars` checks the inner of
  every square and each side of every product; `ClassifyNormalizedProduct` flattens every `*` tree and counts
  decision leaves. Refused: `x * POWER(y, 2)`, `POWER(x, 2) * POWER(y, 2)`, `POWER(POWER(x, 2), 2)`, `x * x * y`, an
  exponent other than 2, and `x * x` (spell it `POWER(x, 2)`). Without the guard a squared factor would be read as a
  data coefficient and give a wrong Q.
- Nothing expands a term before binding, and the canonicalizer never opens one, so the square the extractor matches
  always survives intact.

## Products of two decisions

- **`BOOL × BOOL`** becomes an AND: a `BOOL` auxiliary `w` with three rows (`w <= b1`, `w <= b2`,
  `w >= b1 + b2 - 1`). No Big-M.
- **`BOOL × x`** with `x ∈ [L, U]` becomes a McCormick envelope, exact for a binary factor:
  - `w <= U·b`, `w >= x - U·(1 - b)`, `w <= x - L·(1 - b)`, and `w >= L·b` when `L < 0`.
  - The corners are emitted at execution, where `L` and `U` are resolved. For `L >= 0` the last corner is implied by
    `w >= 0` and the third is `w <= x`. Emitting `w <= x` earlier, at optimizer time, would force `x >= 0` on a
    signed `x` when `b = 0`. For `L < 0`, `w`'s own lower bound widens to `L`.
  - `U` must be finite: written, or inferred by implied-bound propagation from a non-negative `<=`/`=` clause such as
    `SUM(x) <= K`. Propagation skips a signed `x`, so a signed factor needs a written upper bound.
  - `w` is declared `INTEGER` when `x` is integer, so a strict bound or `<>` on the product keeps its integer-step
    rewrite (`IsEvalConstraintLhsIntegerValued`).
- **Neither factor `BOOL`** leaves the product in place as off-diagonal Q entries
  (`q[{max(a,b), min(a,b)}] += coeff`). The result is always indefinite.
- **Data factors** on either side (`(a * x) * (b * y)`, `profit * b * x`, `(a*b)*(x*y)`) are collected by
  `ClassifyProduct`, which flattens the tree into decision factors and data factors; `BuildCoefficientFromFactors`
  re-binds each `*` it rebuilds through `DecideRebindOperator`, because reusing the original multiply's signature over
  reshaped operands misreads their physical types.
- Products compose with `when`, `per`/`by`, a square in the same objective, and linear terms.

## Code

| What | Where |
|---|---|
| Binder checks for squares and products | `ValidateSumArgumentInternal` (`decide_binder.cpp`); constraints pass `allow_bilinear` |
| Term splitting, square detection, degree guard | `DecideTermSplitter::Split`, `DetectQuadraticPattern`, `ClassifyProduct` (`decide_term_split.cpp`) |
| Prepared terms | `ExtractLinearAndBilinearTerms`, `ExtractConstraintTerms` (`decide_linear_form.cpp`) |
| Product rewrite | `RewriteBilinear` (`decide_rewrite_bilinear.cpp`), after `RewriteAbs`, before `RewriteMinMax`; `LogicalDecide::BilinearLink`, `is_boolean_var` |
| McCormick corners | `physical_decide.cpp` |
| Q and quadratic constraints | `ilp_model_builder.cpp` (`BuildQuadraticConstraint`, `nonconvex_quadratic`) |
| Solvers | `gurobi_solver.cpp` (`GRBaddqpterms`, `GRBaddqconstr`, `NonConvex=2`); `deterministic_naive.cpp` (HiGHS, `passHessian`) |
| Tests | `test_bilinear.py`, `test_quadratic.py`, `test_quadratic_constraints.py`, `test_cons_comparison.py::test_bilinear_bool_int_strict_oracle` |
