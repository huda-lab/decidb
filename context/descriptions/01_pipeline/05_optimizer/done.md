# Stage 05 — DECIDE optimizer

Chooses the **mathematical formulation** for every construct the solver cannot take
literally: ABS, MIN/MAX, AVG, `<>`, bilinear products, and composed reducers. It
assumes canonical input and produces canonical output; it never decides shape,
parses SQL, executes relations, or calls a solver.

**Key source files**

- `src/optimizer/decide/decide_optimizer.cpp` (~1,700 lines)
- `src/include/duckdb/optimizer/decide_optimizer.hpp`

Every constraint it emits re-enters through `LogicalDecide::AddConstraint` and
every objective through `LogicalDecide::SetObjective`, both of which
re-canonicalize and re-verify. `AppendConstraint` is a thin forwarder to the
former.

---

## 1. Pass order

`DecideOptimizer::OptimizeDecide` runs nine passes, and the order is load-bearing:

| # | Pass | Why here |
|---|---|---|
| 1 | `RewriteNorm` | Lowers bound NORM markers before later passes see their ABS, POWER, MAX, or L0 links. |
| 2 | `RewriteInDomain` | Lowers bound DECIDE-variable IN markers to their indicator formulation. |
| 3 | `TagAbsConstraintsForBigM` | Marks the ABS nodes that will need a Big-M envelope. Must precede `RewriteAbs`, which replaces the nodes. |
| 4 | `RewriteAbs` | Creates auxiliaries replacing ABS nodes; must be first of the remaining rewrites so later passes see plain variables. |
| 5 | `RewriteBilinear` | McCormick linearization for Boolean × anything. |
| 6 | `RewriteComposedMinMax` | Detects composed (additive, mixed-reducer) MIN/MAX **before** single-term MIN/MAX handling. |
| 7 | `RewriteMinMax` | Classifies and rewrites single top-level MIN/MAX in constraints and objectives. |
| 8 | `RewriteNotEqual` | Creates `<>` indicators. |
| 9 | `RewriteAvgToSum` | Last, so every reducer that reaches it is settled. |

`RewriteComposedMinMaxObjectiveTop` runs within the composed pass for the
objective side. Setting `DECIDB_BENCH` prints `optimizer_ms` for the whole block.

---

## 2. The passes

### ABS linearization — `RewriteAbs`

Detects `BoundFunctionExpression` for `ABS` over decision variables, creates
auxiliary `REAL` variables, replaces the ABS nodes with references to them, and
emits `aux >= inner` and `aux >= -inner`.

`AbsPairInfo::needs_bigm` records whether the auxiliary needs an upper envelope.
It is false when solver pressure pins `aux` to `|inner|` naturally — a `MINIMIZE`
objective, or a constraint shape that already upper-bounds `aux`. When true
(constraint hard direction, or `MAXIMIZE` + objective), `abs_maximize_links`
carries the auxiliary and a binary sign indicator, and execution derives the two
upper-bound rows from the tagged lower-bound constraints.

`TagAbsConstraintsForBigM` decides this by reading the constraint as `E <op> 0`
and asking whether each ABS term's signed position pushes its auxiliary down.
`CollectAbsWithSign` folds signs through `+`, unary and binary `-`, casts,
aggregate bodies, and constant factors. A factor whose value is not known until
execution — a data column, as in `SUM(w * ABS(x - t))` — yields sign 0, which
never matches the pinning direction and so forces Big-M.

That is deliberately conservative: a row whose `w` happens to be positive gets
an envelope it does not need. Assuming such a factor positive is unsound, since
a negative `w` makes enlarging the auxiliary *relax* the row, and the constraint
silently stops binding. The cost was measured 2026-08-14 on a weighted-ABS
constraint over TPC-H `lineitem`, comparing a literal coefficient (pinned, no
Big-M) against an all-ones column (Big-M) so the two models are mathematically
identical: 0.16s vs 0.65s at 30K rows and 0.26s vs 1.29s at 60K, same optimum
both ways. A ~5x constant factor, still near-linear in row count — cheap enough
that deciding sign per row at execution time is not warranted.

### NORM and IN — `RewriteNorm` / `RewriteInDomain`

The binder keeps `norm(...)` as an aggregate-shaped DECIDE marker so normal
aggregate-local `WHEN` and `PER` binding remains available. The optimizer lowers
L1, L2, and infinity norms to `SUM(ABS)`, `SUM(POWER(_, 2))`, and `MAX(ABS)`;
L0 emits its existing Boolean indicator and exact forward/reverse links.

A bound `x IN (...)` stays a native `COMPARE_IN` marker until this pass. It then
uses the existing singleton, Boolean-domain, or cardinality/linking formulation,
copying an expression-level `WHEN` to every generated row. These helper rows are
structural for today’s infeasibility repair: they must never be independently
loosened. Atomic source-level DROP repair is recorded in `todo.md`.

### Bilinear — `RewriteBilinear`

| Operand types | Formulation |
|---|---|
| Boolean × Boolean | AND-linearization: `w <= b1`, `w <= b2`, `w >= b1 + b2 - 1` |
| Boolean × other | Partial McCormick: `w <= x` at plan time, `w <= U·b` and `w >= x - U·(1-b)` at execution time via `BilinearLink` (needs runtime bounds) |
| Non-Boolean × non-Boolean | Left in place for the Q-matrix path in the physical operator |

`ExtractMultiplicativeCoefficient` walks a chain shaped
`coeff * ... * decide_var * ... * coeff` and combines the non-variable factors.

### MIN/MAX — `RewriteMinMax`

**Constraints.** An *easy* direction (`MAX(...) <= K`, `MIN(...) >= K`) strips the
reducer and becomes a per-row constraint — one row per data row, no Big-M. A *hard*
direction creates a Boolean indicator per active row and rewrites to
`SUM` + linking rows (`EmitHardMinMaxIndicator`). Equality splits into both
directions. `WHEN` / `PER` wrappers are preserved; `out_was_easy` tells the caller
whether `PER` should be stripped, since an easy rewrite has already become
per-row.

**Objectives.** Flat and nested-`PER` patterns are detected and the easy/hard
classification is precomputed into typed metadata — `flat_objective_agg` /
`flat_objective_is_easy`, and `per_inner_agg` / `per_outer_agg` /
`per_inner_is_easy` / `per_outer_is_easy` / `per_inner_was_avg` — so the physical
layer reads the decision rather than re-deriving it. `RewriteMinMaxObjectiveTree`
is split out from `RewriteMinMaxObjective` because the rewrite writes through a
pointer that walks *into* the tree past `PER`/`WHEN` wrappers and casts, and has
early returns: the objective must be detached, rewritten, and reinstalled through
`SetObjective`.

### Composed MIN/MAX — `RewriteComposedMinMax`

An additive LHS mixing `SUM`/`AVG` with `MIN`/`MAX` terms cannot go through the
single-term path. Each term's metadata is extracted into
`composed_minmax_constraints` (or `composed_minmax_objective_terms`) as a
`ComposedMinMaxTerm`, and the comparison is replaced with a no-op `TRUE`
placeholder so the physical layer owns emission. Non-comparison leaves are legal
under C0 precisely so that placeholder verifies.

For the objective, `RewriteComposedMinMaxObjectiveTop` installs a **constant
placeholder** and supplies the coefficients from
`composed_minmax_objective_terms` instead — which is why a decision-free objective
is legal and returned unchanged by the canonicalizer.

### `<>` — `RewriteNotEqual`

Each `COMPARE_NOTEQUAL` gets a Boolean indicator recorded in
`ne_indicator_indices`. The Big-M disjunction rows are generated at execution
time, once bounds are known.

### AVG → SUM — `RewriteAvgToSum`

Replaces a decision-bearing `AVG` with `SUM`, tagged `AVG_REWRITE_TAG` so
coefficient evaluation scales the terms by the right active-row count (total,
`WHEN` mask, group size, or aggregate-local filter). A **decision-free** `AVG` is
skipped: there is nothing to linearize, and rebinding it as `SUM` would redeclare
it with `SUM`'s integral type while its value stays fractional. Keeping the tag on
objective terms is what makes a mixed `AVG(a) + SUM(b)` preserve true AVG
semantics.

---

## 3. A factor on a reducer stays outside

The canonicalizer peels a query-wide factor *outward* off a reducer
(`2 * SUM(x*p)`, `2 * MAX(x*v)`) and converges every spelling onto one. **No pass
here pushes it back in.**

`MIN`/`MAX` are order statistics, so a negative factor turns one into the other —
`MAX(-2x)` is `-2·MIN(x)`. Folding therefore needs the factor's sign, which an
uncorrelated scalar subquery does not supply until the query runs. Kept outside,
the sign is not a correctness input at all; it only selects which linearization is
cheaper. `ScaleSignAtPlanTime` returns +1, -1, or **0 for "not known until the
query runs"**, and 0 must be treated as "take the expensive but exact form", never
as an error. A scale of -2 flips the direction `z` is pushed exactly as a
subtraction would, so a known sign participates in `is_easy` alongside `sign`;
an unknown sign forces `is_easy` false, and the indicator layer pins `z` to the
true MIN/MAX in both directions — correct whichever sign it turns out to have.

`WalkComposedLhs` records the factor on `ComposedMinMaxTerm::scale` (with
`scale_divides` for the `AGG / factor` spelling). The physical layer applies it
where the term lands: multiplied into per-row coefficients for `SUM`/`AVG`
(`ApplyScaleToExtracted`), or into the auxiliary's contribution for `MIN`/`MAX`,
or distributed over the per-row form when the constraint linearizes the easy way.

A parsed-level fold used to do this **without** the sign check. It was a silent
wrong answer.

---

## 4. Optimizations not in this pass

Two things reduce problem size before the optimizer runs, and are worth knowing
because they dominate everything above:

- **`WHERE`-clause filtering** — standard DuckDB predicate pushdown, no
  DECIDE-specific code. Rows eliminated by `WHERE` never become decision
  variables, which for most queries is the single most impactful optimization
  there is.
- **`WHEN`-condition coefficient zeroing** — stage 08. Aggregate constraints
  multiply coefficients by the mask; per-row constraints omit the row entirely;
  objectives zero the coefficient. `WHEN`-excluded rows get `INVALID_INDEX` in
  `row_group_ids`, so they fall out of every group.

Backend selection (Gurobi preferred, HiGHS fallback) and the shared time limit are
stage 07.

---

## 5. What this stage does not do

- Decide which side of a comparison a term sits on — stage 04. Every pass here was
  audited against that: ABS and bilinear replace decision-bearing atoms with
  decision auxiliaries; MIN/MAX and AVG keep decision terms on the left; `<>`
  changes only metadata; composed MIN/MAX leaves a permitted placeholder.
- Evaluate anything against data, or emit solver rows.

---

## 6. Source map

| Concern | Location |
|---|---|
| All passes | `src/optimizer/decide/decide_optimizer.cpp` |
| Pass inventory and helper contracts | `src/include/duckdb/optimizer/decide_optimizer.hpp` |
| Metadata the passes write | `src/include/duckdb/planner/operator/logical_decide.hpp` |
| Canonicalizing entry points | `src/planner/operator/logical_decide.cpp` |
| Per-function user-facing semantics | `../../03_expressivity/sql_functions/done.md` |
| Bilinear semantics | `../../03_expressivity/bilinear/done.md` |
