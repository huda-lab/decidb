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

The optimizations in this area aim to make the ILP smaller and safer to solve —
fewer constraints, tighter bounds, and configurable time limits.
