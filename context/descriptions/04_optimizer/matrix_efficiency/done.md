# Matrix Efficiency — Done

## Data-Driven Big-M + Implied-Bound Propagation

DeciDB derives the Big-M constants used by its linearizations from the actual
variable bounds and per-row coefficient data, rather than a fixed `1e6`. Big-M
is required wherever a constraint/objective is toggled by a binary indicator:
`<>` (not-equal), hard-direction `MIN`/`MAX` constraints (`MAX(expr) >= K`,
`MIN(expr) <= K`, equality), and hard `MIN`/`MAX` objectives
(`MAXIMIZE MAX`, `MINIMIZE MIN`). A constant that is too large hurts numerical
stability ("trickle flow" through the solver's feasibility tolerance); one that
is too small silently distorts the feasible region. A data-derived value avoids
both.

### How M is computed

- **Per-row indicator sites** (`<>`, hard `MIN`/`MAX` constraints): the tight
  scalar `M = maxₐ꜀ₜᵢᵥₑ ᵣₒw ( |rhs[r]| + Σ_t |coef[t][r]|·max(|lb_t|,|ub_t|) ) + 1`.
  The `+1` covers the integer-step band of the `<>` rewrite. Computed by
  `DecideTightPerRowBigM` (`src/execution/operator/decide/physical_decide.cpp`),
  built on `DecideRowTermRange`.
- **Aggregate `<>`** (`SUM(x) <> K`, `AVG(x) <> K`): `M` is computed **per group
  by summing the worst-case contribution over that group's rows**, because the
  aggregate LHS ranges over the whole group. A single per-row bound (the previous
  behavior, floored to `1e6`) is far below the true range at scale and silently
  caps the aggregate — a correctness bug this fixes. See the deferred-NE
  expansion in `physical_decide.cpp`.
- **Hard `MIN`/`MAX` objective auxiliaries** (`compute_big_m`): these link an
  auxiliary `z`/`z_g`/`w` to the objective expression via
  `(aux - expr) ± M·y (≷) ±M`. The deactivated branch must cover the **global
  spread** of `(aux - expr)` across rows, i.e. `maxᵣ exprmaxᵣ − minᵣ exprminᵣ`,
  using each coefficient's **sign** against `[lb, ub]`. (A per-row magnitude can
  under-estimate this when coefficient signs differ across rows.) The legacy `1e6`
  floor is retained only as a fallback when a contributing variable is unbounded.
- **McCormick bilinear envelopes** (`w <= U·b`, `w >= x − U·(1−b)`) and the
  **ABS sign-indicator** linearization already used the real variable bound `U`;
  these are unchanged in spirit and now benefit from tighter inferred bounds.

### Implied-bound propagation

`DecidePropagateImpliedBounds` (`physical_decide.cpp`) derives finite upper
bounds for otherwise-unbounded variables from non-negative `<=`/`=` constraints
(the knapsack/budget pattern): for `Σ_t a_t x_t (<=|=) K` with every `a_t >= 0`
and `x_t >= 0`, each instance satisfies `x ≤ K / a` (the other non-negative terms
only help), so a sound shared upper bound is `maxᵣ (K_r / a_r)` over the
variable's rows. The tightened bounds are written into
`solver_input.upper_bounds` before the Big-M / McCormick values are computed.

**Soundness guards** (each is required; violating any silently cuts the optimum):
- All coefficients in the constraint must be `>= 0` (negative coefficients —
  e.g. the `IN`/`ABS` rewrites `x − z₁ − 3·z₂ = 0` — would make dropping a term
  invalid).
- All variable lower bounds must be `>= 0`.
- The constraint must apply to **every** row, and the variable must have a
  **non-zero coefficient in every row** (`every_row_constrained`): a
  `WHEN`-conditional or otherwise-partial constraint leaves the excluded rows
  unconstrained, but the bound is shared across all of a variable's rows.
- Per-row RHS `K_r >= 0`.

Propagation is a single pass (sound; it does not chase chained implications to a
fixpoint). Only provably-implied bounds are applied, so the feasible region — and
the optimum — are unchanged.

### User-visible effect

Because an implied bound can now be *derived*, some queries that previously had
to be rejected for lack of a finite bound now solve:
- `MAXIMIZE SUM(ABS(x − col))` with `SUM(x) = K` (no explicit `x <= …`) — the
  bound `x ≤ K` is inferred and the ABS sign-indicator Big-M becomes finite.
- Bool×Non-Bool bilinear `SUM(b * x)` with `SUM(x) <= K` — McCormick gets its
  required finite `U`.

No query that solved before changes its optimum; the differential test suite
(which checks optima against an independent oracle) is the regression guard, plus
targeted tests in `test_implied_bounds.py`, `test_min_max.py`
(`test_maximize_max_mixed_sign_coefficient`, `test_maximize_max_max_per_mixed_sign`),
and `test_abs_linearization.py` (`test_abs_maximize_bound_inferred_from_sum`).

---

## Solver Time Limit (Gurobi)

The Gurobi backend caps solve time so a hard MIQP/QCQP cannot hang the session indefinitely.

- **Default**: 300 seconds, applied via `TimeLimit` on the Gurobi environment before `startenv`.
- **Override**: the `DECIDB_TIME_LIMIT` environment variable (seconds, a positive double). Unparseable or non-positive values are ignored and the default is kept.
- **On timeout**: Gurobi returns the best feasible incumbent found so far; the `GRB_TIME_LIMIT` status branch in the result handler surfaces it rather than failing silently.

**Code pointer**: `src/decidb/gurobi/gurobi_solver.cpp` (`GurobiSolver::Solve`, environment setup — the `TimeLimit` / `DECIDB_TIME_LIMIT` block, and the `GRB_TIME_LIMIT` status branch).

The HiGHS fallback also enforces a limit: `RunAndReadback` calls `highs.setOptionValue("time_limit", time_limit_seconds)` per chunk (`src/decidb/naive/deterministic_naive.cpp`), sharing the same budget the facade resolves for Gurobi, and returns the best feasible incumbent on `kTimeLimit`. Neither backend can hang the session on a hard model.

---

## Constraint-to-Bound Absorption

Single-variable constraints are absorbed directly into a variable's column bounds instead of being emitted as matrix rows. A bound is O(1) per variable in the solver, whereas each matrix constraint adds a row to the tableau — so `x <= 5` should never cost a row.

**What is absorbed** (`TraverseBoundsConstraints`, `physical_decide.cpp`): a comparison whose LHS is a bare DECIDE variable (cast-wrapped is unwrapped) and whose RHS is a constant.
- `x <= K` → upper bound; `x >= K` → lower bound; `x = K` → intersect both (never overwrite, so `x = 5 AND x = 10` correctly inverts the box and is caught).
- `x < K` / `x > K` on an INTEGER/BIGINT variable → normalized to `x <= K-1` / `x >= K+1`; a REAL strict inequality is left for the constraint path to reject.
- `x BETWEEN a AND b` → both bounds at once.
- Multiple constraints on one variable take the **tightest** (`std::min`/`std::max` combiners).

**What is NOT absorbed** (stays a per-row/matrix constraint, by design):
- **WHEN-conditional** bounds (`x <= 0 WHEN cond`) — the `WHEN_CONSTRAINT_TAG` branch is skipped entirely, so a conditional bound never leaks into the global column bound; it is applied per matching row instead.
- **PER** wrappers recurse only into the wrapped constraint, not the grouping columns.
- **Multi-variable** LHS (e.g. `x - 3*z₁ - 5*z₂ = 0` from the `IN` rewrite) — only a single bare variable qualifies.

**Mechanism**: the absorption runs in `DecideGlobalSinkState`'s constructor, filling `absorbed_lower_bounds` / `absorbed_upper_bounds` (lower initialized to the `ABSORBED_LOWER_UNSET` sentinel so an explicit negative bound is honored rather than clamped to 0). Absorbed comparison expressions are recorded in `absorbed_bound_exprs` and skipped during matrix emission. Each absorbed direction is also recorded as a `UserBoundSpec` in `user_absorbed_bounds` so the infeasible-diagnosis engine can re-emit it as a slackable `USER_PARAMETER` row — the bound stays visible to diagnostics even though it never became a matrix row. A user bound that contradicts a variable's intrinsic domain (a non-negative variable's `<= -1`, a BOOLEAN's `>= 2`) raises a precise static error here rather than reaching the elastic engine.

**Tests**: correctness is oracle-verified in `test_cons_perrow.py` and `test_var_real.py` / `test_var_multi.py`; the WHEN-not-globally-absorbed contract in `test_when_perrow.py` (`x <= 0 WHEN …`, `x = 1 WHEN …`); the diagnostics re-emission in `test_error_infeasible.py`.

Beyond this and the implied-bound propagation above, no further matrix-level structural reduction (row pruning / constraint push-down) is implemented — those remain in `../rewrite_passes/todo.md` and `../future_work/todo.md`. Both Gurobi and HiGHS still run their own internal presolve on whatever matrix `SolverModel::Build()` hands off.
