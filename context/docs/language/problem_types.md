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
| any | a square with a weight known only when the query runs (`w * POWER(e, 2)`) | non-convex QP | yes (`NonConvex=2`) | refused |
| mixed | quadratic objective | MIQP | yes | refused |
| any, one factor `BOOL` | `b * x` | MILP (McCormick) | yes | yes |
| any, no `BOOL` factor | `x * y` | non-convex QP | yes | refused |
| any | `POWER(e, 2)` or `x * y` in `SUCH THAT` | QCQP | yes (`GRBaddqconstr`) | refused |
| any | no objective | feasibility | yes | yes |

The table is `SolverModelClass` (`src/include/duckdb/common/decide_solver_capabilities.hpp`) read as a matrix, and
`ClassifyDecideModel` is the one function that classifies a model's shape into it. A
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

- **Spellings.** `POWER(e, 2)`, `e ** 2` and `(e) * (e)` are the same square, and so is any product of equal factors
  (`2*x*x`, `x*2*x`, `(x+y)*(y+x)`): canonicalization writes each as a coefficient times `POWER(e, 2)`. A minus sign or
  a factor in front is the square's weight (`-POWER(e, 2)`, `K * POWER(e, 2)`, `w * POWER(e, 2)`). The inner `e` is
  linear in the decisions.
- **Objective.** `Σ w·(a·x + c)²` becomes `½ xᵀQx + cᵀx` with `Q[i,j] = w · 2·aᵢ·aⱼ`, summed over rows, and
  the cross terms `w · 2·c·aᵢ` going to the linear part. `MINIMIZE` a sum of squares is convex; `MAXIMIZE` its
  negation is concave and both solvers take it; `MAXIMIZE` a sum of squares is non-convex. Q is kept in COO form and
  converted to CSC for HiGHS (`passHessian`).
- **Several squares.** Each square is one product group, `Q += w · l · rᵀ` (`DecideProductGroup`), so an objective
  holds any number of squares, beside any number of linear terms, summed into one Q: `SUM(POWER(x - a, 2)) +
  SUM(POWER(y - b, 2))` answers. Each constraint carries its own groups, accumulated into one Q.
- **The weight's sign decides convexity, per group.** A square is convex when the sign of its weight agrees with the
  sense. A constant weight has a sign the plan knows. A weight known only when the query runs (a data column, a
  query-wide subquery) has none, so the group counts as non-convex: Gurobi solves it as it solves `x * y`, and HiGHS
  refuses it at plan time.
- **Constraints.** `SUM(POWER(e, 2)) <= K` or per row `POWER(e, 2) <= K`. Constants of the inner move to the bound.
  Quadratic constraints compose with scopes and with linear terms in the same clause.
- **Under MIN or MAX.** A square or product inside a `MIN`/`MAX` (`MAX(POWER(x - t, 2)) >= K`,
  `MINIMIZE MAX(POWER(x - t, 2))`), a squared `SUM` beside one (`SUM(POWER(x, 2)) + MAX(x) <= K`), or the inner
  reducer of a two-level objective under one (`MAX(per g: SUM(POWER(x - t, 2)) by (g))`): each member becomes a
  helper decision per row with one quadratic row, and the extremum is stated to the solver natively, so it is Gurobi
  only; HiGHS refuses, naming the reducer. `MAX(POWER(e, 2)) <= K` and `MIN(POWER(e, 2)) >= K` need no helper: they
  are one quadratic row per member.
- **Degree guard.** The total decision degree of a term is at most 2. The shape check judges it once, on the canonical
  spelling, by `DecideDegreeOf` (stage 04): a `*` adds its factors' degrees and a whole-exponent `POWER` multiplies its
  base's. Refused: `x * POWER(y, 2)`, `POWER(x, 2) * POWER(y, 2)`, `POWER(POWER(x, 2), 2)`, `x * x * y`, `POWER(x, 3)`,
  and a `POWER` whose exponent is not a whole number known before the query runs. Accepted: `x * x`, `2*x*x`,
  `POWER(x, 1)` (which is `x`) and `POWER(x, 0)` (which is 1). Without the guard a squared factor would be read as a
  data coefficient and give a wrong Q.
- The canonicalizer never multiplies out a square or a power, so the square the term splitter reads always survives
  intact.

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
- **Data factors** on either side (`(a * x) * (b * y)`, `profit * b * x`, `(a*b)*(x*y)`) are composed into the
  product's coefficient by `NormalizeProducts`, which never merges or folds them, and the term splitter reads the
  coefficient and the two decision factors (`ReadDecideProduct`). Each `*` it rebuilds is re-bound through
  `DecideRebindOperator`, because reusing the original multiply's signature over reshaped operands misreads their
  physical types.
- Products compose with `when`, `per`/`by`, a square in the same objective, and linear terms.

## Code

| What | Where |
|---|---|
| The one spelling of a square or product, and its degree | `NormalizeProducts`, `DecideDegreeOf` (`decide_product_form.cpp`) |
| Degree guard and the other refusals of algebra | `DecideShapeCheck` (`decide_shape_check.cpp`) |
| Term reading | `DecideTermSplitter::Split` (`decide_term_split.cpp`) |
| Prepared product groups | `ExtractObjectiveBodyTerms`, `ExtractConstraintTerms`, `ReadProductGroup` (`decide_linear_form.cpp`) |
| Product rewrite | `RewriteBilinear` (`decide_rewrite_bilinear.cpp`), after `RewriteAbs`, before `RewriteMinMax`; `LogicalDecide::BilinearLink`, `is_boolean_var` |
| McCormick corners | `physical_decide.cpp` |
| Q and quadratic constraints | `ilp_model_builder.cpp` (`AddProductGroupRow`, `BuildQuadraticConstraint`, `nonconvex_quadratic`) |
| Model class | `ClassifyDecideModel`, `IsNonconvexProductGroup` (`decide_solver_capabilities.hpp`) |
| Solvers | `gurobi_solver.cpp` (`GRBaddqpterms`, `GRBaddqconstr`, `NonConvex=2`); `deterministic_naive.cpp` (HiGHS, `passHessian`) |
| Tests | `test_bilinear.py`, `test_quadratic.py`, `test_quadratic_constraints.py`, `test_cons_comparison.py::test_bilinear_bool_int_strict_oracle` |
