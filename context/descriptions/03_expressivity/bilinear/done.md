# Bilinear Terms (`x * y`) — Implemented Features

Products of two different DECIDE variables are supported in both objectives and constraints. The behavior depends on whether one of the variables is Boolean.

---

## Two Categories

### 1. Boolean x Anything (McCormick Linearization)

When one factor is declared `IS BOOLEAN`, the product `b * x` is exactly linearized using McCormick envelopes. This produces an equivalent MILP reformulation — no relaxation, exact for binary variables. Works with both Gurobi and HiGHS.

**Requires**: A finite upper bound on the non-Boolean variable. This may be given
explicitly (`x <= K`) **or inferred** by implied-bound propagation from a
non-negative `<=`/`=` constraint such as `SUM(x) <= K` (which implies `x <= K`).
Only when no finite bound can be derived does DeciDB reject with the "finite upper
bound" error. See `../../04_optimizer/matrix_efficiency/done.md`.

**Bool x Bool** uses simpler AND-linearization (no Big-M needed). The auxiliary `w` satisfies `w = min(b1, b2)`, i.e., logical AND.

**Bool x Non-Bool** uses McCormick envelopes with Big-M:
- Structural constraint `w <= x` (generated at optimizer time)
- Big-M constraints `w <= U*b` and `w >= x - U*(1-b)` (generated at execution time, where U = upper bound on x)

### 2. General Non-Convex (Q Matrix)

When neither factor is Boolean (`Real*Real`, `Int*Int`, `Int*Real`), the product produces off-diagonal entries in the Q matrix. This is always indefinite (non-convex).

- **Objectives**: Gurobi only (via `NonConvex=2`). HiGHS rejects with a clear error.
- **Constraints**: Gurobi only (via `GRBaddqconstr`). HiGHS rejects with a clear error.

---

## Syntax

Bilinear terms appear naturally in `SUM()` expressions:

```sql
-- Boolean x Real objective (McCormick, both solvers)
MAXIMIZE SUM(b * x)

-- With data coefficient
MAXIMIZE SUM(profit * b * x)

-- Boolean x Boolean constraint (AND-linearization, both solvers)
SUCH THAT SUM(b1 * b2) <= 5

-- Bilinear objective with coefficients on both factors
MAXIMIZE SUM((a * x) * (b * y))

-- Bilinear constraint with coefficients on both factors
SUCH THAT (a * x) * (b * y) >= 10

-- Non-convex objective (Gurobi only)
MINIMIZE SUM(x * y)

-- Mixed linear + bilinear
MAXIMIZE SUM(cost + b * x)
```

### Composability

Bilinear terms compose with existing features:
- **WHEN**: `MAXIMIZE SUM(b * x) WHEN category = 'A'`
- **PER**: `MAXIMIZE SUM(b * x) PER grp` (covered by `test_bilinear_per_group`)
- **Mixed with POWER**: `MINIMIZE SUM(POWER(x - target, 2) + b * x)` (bilinear + quadratic in same objective)

### Degree Guard (Total Degree ≤ 2)

Only bilinear (`x * y`, different variables, each linear in decide vars) and quadratic (`POWER(linear_expr, 2)`) are supported. Products with total decision-variable degree > 2 are rejected at execution time by `ClassifyNormalizedProduct` (which flattens all `*` nodes and counts decide-variable leaves). Self-products of the same variable are also rejected. Rejected shapes include:

- `x * POWER(y, 2)` (cubic) — variable × squared
- `POWER(x, 2) * POWER(y, 2)` (quartic) — squared × squared
- `POWER(x, 2) * POWER(x, 2)` (x⁴) — identical squared self-product
- `x * x * y`, `POWER(POWER(x, 2), 2)` (x⁴), etc.

Without this guard the bilinear emitter would silently treat the inner POWER / nested-`*` factor as an opaque "data coefficient", producing a wrong Q matrix or crashing the coefficient evaluator. See `problem_types/done.md` → "Quadratic objective detection" Code Pointer for the shared helper and the corresponding test rows in `05_testing/quadratic/done.md`.

---

## Implementation Architecture

### Pipeline Flow

1. **Binder** (`decide_binder.cpp`): Relaxed validation to allow `decide_count == 2` products when `allow_quadratic` or `allow_bilinear` is true. Triple products (`a * b * c`) rejected. `allow_bilinear` parameter added for constraints (separate from `allow_quadratic` to prevent POWER in constraints).

2. **Symbolic** (`decide_symbolic.cpp`): Bilinear expressions now go through SymEngine's normal `expand().simplify()` path. The previous bypass (`SumInnerContainsBilinear`) was removed because the physical extractor's flat-factor classifier handles any post-expansion shape. Only quadratic expressions (`POWER` / self-product) still bypass SymEngine (to preserve the recognizable QP structure).

3. **Optimizer** (`decide_optimizer.cpp`): `RewriteBilinear()` pass runs after `RewriteAbs`, before `RewriteMinMax`. Walks both objective and constraint expressions:
   - Detects `*` nodes where both children reference different decide variables
   - Skips identical expressions (existing QP path)
   - For Bool x Bool: AND-linearization with 3 structural constraints, BOOLEAN auxiliary
   - For Bool x Non-Bool: structural constraint `w <= x` + `BilinearLink` for execution-time Big-M
   - For Non-Boolean x Non-Boolean: left in place for Q matrix path
   - Uses `is_boolean_var` vector (not `return_type`) to detect boolean status

4. **Physical Operator** (`physical_decide.cpp`):
   - `ExtractLinearAndBilinearTerms()`: separates linear and bilinear terms in objectives
   - `ExtractConstraintTerms()`: same for constraints
   - `ClassifyNormalizedProduct()`: flattens any nested `*` tree into leaf factors, partitions them into decide-variable indices (`decide_factors`) and data expressions (`coefficient_factors`). Handles arbitrary groupings like `(a*b)*(x*y)` and `a*b*x*y` identically.
   - `BuildCoefficientFromFactors()`: rebuilds the coefficient sub-expression from the data leaf factors, used for bilinear terms.
   - Linear terms (`decide_factors.size() == 1`) fall through to `ExtractTerms` (uses `ExtractCoefficientWithoutVariable` on the original tree for type-safe coefficient extraction).
   - McCormick Big-M generation: uses `BilinearLink` metadata + `ExtractVariableBounds` to generate `w <= U*b` and `w >= x - U*(1-b)` constraints
   - Evaluates bilinear coefficients per-row, applies WHEN mask

5. **Model Builder** (`ilp_model_builder.cpp`):
   - Bilinear off-diagonal Q entries: `q_map[{max(flat_a,flat_b), min(flat_a,flat_b)}] += coeff`
   - Quadratic constraints: `QuadraticConstraint` struct with separate linear and quadratic parts
   - Skips `INVALID_INDEX` variable entries (constant data terms in mixed objectives)

6. **Solvers**:
   - Gurobi: `GRBaddqpterms` for Q matrix (existing), `GRBaddqconstr` for quadratic constraints (new)
   - HiGHS: rejects non-convex Q and quadratic constraints with clear errors

### Key Data Structures

- `LogicalDecide::BilinearLink` — `{aux_idx, bool_var_idx, other_var_idx}` for execution-time Big-M
- `LogicalDecide::is_boolean_var` — per-variable boolean flag (since `return_type` is INTEGER for all non-REAL vars)
- `SolverInput::BilinearObjectiveTerm` — `{var_a, var_b, row_coefficients}`
- `SolverInput::EvaluatedConstraint::BilinearTerm` — same structure for constraints
- `SolverModel::QuadraticConstraint` — linear + quadratic parts for `GRBaddqconstr`

---

## Code Pointers

- **Binder validation**: `src/planner/expression_binder/decide_binder.cpp` — `ValidateSumArgumentInternal()`, `allow_bilinear` parameter
- **Constraint binder**: `src/planner/expression_binder/decide_constraints_binder.cpp` — passes `allow_bilinear=true`
- **Symbolic normalization (bilinear)**: `src/decidb/symbolic/decide_symbolic.cpp` — bilinear expressions now use the default SymEngine expansion path; `ComparisonLhsHasQuadratic` bypasses SymEngine only for quadratic (POWER) shapes
- **Optimizer rewrite**: `src/optimizer/decide/decide_optimizer.cpp` — `RewriteBilinear()`, `FindAndReplaceBilinear()`
- **Boolean type tracking**: `src/include/duckdb/planner/operator/logical_decide.hpp` — `is_boolean_var`
- **Bilinear link struct**: `src/include/duckdb/planner/operator/logical_decide.hpp` — `BilinearLink`
- **Physical execution**: `src/execution/operator/decide/physical_decide.cpp` — `ExtractLinearAndBilinearTerms()`, `ExtractConstraintTerms()`, `ClassifyNormalizedProduct()`, `BuildCoefficientFromFactors()`, McCormick Big-M generation
- **Model builder**: `src/decidb/utility/ilp_model_builder.cpp` — Q matrix off-diagonal entries, `QuadraticConstraint` building
- **Gurobi quadratic constraints**: `src/decidb/gurobi/gurobi_solver.cpp` — `GRBaddqconstr` loop
- **HiGHS rejection**: `src/decidb/naive/deterministic_naive.cpp` — quadratic constraint check
- **Serialization**: `src/storage/serialization/serialize_logical_operator.cpp` — properties 225-228

### McCormick auxiliary type preserves integer-valuedness

Inside `FindAndReplaceBilinear` (`src/optimizer/decide/decide_optimizer.cpp:964`), the McCormick auxiliary `w = b * x` is declared `LogicalType::INTEGER` whenever the non-Boolean factor is integer-typed (Bool × Integer → integer-valued product) and `DOUBLE` otherwise. This is load-bearing for the strict-inequality / NE guard at `src/decidb/utility/ilp_model_builder.cpp:332` (`IsEvalConstraintLhsIntegerValued`), which inspects the declared type of every LHS auxiliary to decide whether the integer-step rewrite (`< K → <= K-1`, `<> K` disjunction) is safe. Marking Bool × Integer auxiliaries as DOUBLE would silently disable that rewrite and push otherwise-valid strict-inequality constraints into the rejection path. Covered implicitly by `test/decide/tests/test_cons_comparison.py::test_bilinear_bool_int_strict_oracle`; update both sites together if the McCormick auxiliary classification is ever revisited.

---

## Error Messages

- `"Triple or higher-order products of DECIDE variables are not supported (total degree > 2)"` — three or more vars in a single product (binder-level)
- `"Bilinear term requires a finite upper bound on variable 'x'"` — McCormick needs `x <= K`
- `"Non-convex quadratic objectives require Gurobi"` — Real*Real or Int*Int bilinear on HiGHS
- `"Quadratic/bilinear constraints require Gurobi"` — non-Boolean bilinear in constraints on HiGHS
- `"DECIDE expression contains a product of decision variables with total degree > 2 ..."` — execution-time degree guard in `ClassifyNormalizedProduct` (degree > 2 decide factors in any `*` tree)
- `"DECIDE expression contains a same-variable product that is not in a supported quadratic form ..."` — `ClassifyNormalizedProduct` detects `x * x` (same variable appearing twice in a flat `*` tree); use `POWER(x, 2)` or `(x)*(x)` for quadratic

---

## Tests

`test/decide/tests/test_bilinear.py` covers:
- Bool x Bool objectives (AND-linearization)
- Bool x Real, Bool x Int objectives (McCormick)
- Data coefficient scaling (`profit * b * x`)
- Objective coefficients from both factor sides (`(a*x)*(b*y)`)
- Shape-equivalent objective coefficients (`(a*x)*(b*y)` vs `a*b*x*y`)
- **Grouped data-factor coefficient** (`(a*b)*(x*y)` vs `a*b*x*y` — same optimal objective)
- **SymEngine-expanded bilinear** (`(x+1)*y` = `x*y + y`, `(x+y)*z` = `x*z + y*z`)
- Non-convex objectives (Real*Real, Int*Int, Int*Real — Gurobi only)
- Mixed linear + bilinear objectives
- Bilinear with WHEN filter
- Bool bilinear constraints
- Bilinear constraint coefficients from both factor sides (`(2*x)*(3*y)`, `(a*x)*(b*y)`)
- **Grouped data-factor bilinear constraint** (`(a*b)*(x*y) >= K` aggregate and per-row)
- Error cases (triple product, missing bounds, HiGHS rejection)
- Backward compatibility (POWER, linear, identical multiplication)
