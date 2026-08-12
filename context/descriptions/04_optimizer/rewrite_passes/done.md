# Rewrite Passes — Done

All DECIDE algebraic rewrites are now in the **DecideOptimizer** pass (logical-level transforms after binding). The binder validates and binds expressions; the optimizer rewrites them.

## Current Rewrite Locations

| Rewrite | Stage | Function | File |
|---------|-------|----------|------|
| ABS linearization (full) | Optimizer | `DecideOptimizer::RewriteAbs` | `decide_optimizer.cpp` |
| Bilinear McCormick linearization | Optimizer | `DecideOptimizer::RewriteBilinear` | `decide_optimizer.cpp` |
| MIN/MAX linearization | Optimizer | `DecideOptimizer::RewriteMinMax` | `decide_optimizer.cpp` |
| `<>` disjunction | Optimizer | `DecideOptimizer::RewriteNotEqual` | `decide_optimizer.cpp` |
| AVG → SUM (term scaling metadata) | Optimizer | `DecideOptimizer::RewriteAvgToSum` | `decide_optimizer.cpp` |

The binder recognizes AVG, ABS, MIN, and MAX as valid DECIDE aggregates/functions and binds them into their respective `BoundExpression` nodes. The optimizer then rewrites them:
- **A factor on a reducer** (`2 * SUM(x*p)`, `2 * MAX(x*v)`) is peeled off by the canonicalizer and **stays outside**. No pass pushes it back in. MIN/MAX are order statistics, so a negative factor turns one into the other, and folding would therefore need the factor's sign — which an uncorrelated scalar subquery does not supply until the query runs. Kept outside, the sign only selects which MIN/MAX linearization is cheaper: known sign takes the cheap direction, unknown sign takes the exact envelope+indicator form. `WalkComposedLhs` records it on `ComposedMinMaxTerm::scale`, and the physical layer multiplies it into per-row coefficients (SUM/AVG) or the auxiliary's contribution (MIN/MAX). A parsed-level fold used to do this without the sign check, which was a silent wrong answer — see `../../07_issues/bugs/done.md`.
- **ABS linearization**: Detects `BoundFunctionExpression` for ABS over decide variables. Creates auxiliary REAL variables, replaces ABS nodes with aux var references, and generates linearization constraints (`aux >= inner`, `aux >= -inner`).
- **Bilinear McCormick linearization**: When one factor of a bilinear product `x * y` is BOOL, introduces an auxiliary variable `w = x * y` and emits McCormick envelope constraints (Bool×any) or AND-linearization (Bool×Bool). Non-convex bilinear terms (Real×Real, Int×Real, Int×Int) are left for the Gurobi QCQP path. See `../../03_expressivity/bilinear/done.md`.
- **MIN/MAX linearization**: Classifies MIN/MAX constraints as easy (strip aggregate → per-row) or hard (create BOOL indicator, rewrite to SUM). Handles equality splitting, WHEN/PER wrappers, and objectives (flat and nested PER). Populates `minmax_indicator_links` on `LogicalDecide`. For objectives, sets typed metadata: `flat_objective_agg`/`flat_objective_is_easy` (flat) and `per_inner_agg`/`per_outer_agg`/`per_inner_is_easy`/`per_outer_is_easy` (PER). The easy/hard classification is pre-computed at optimization time so the physical layer reads the decision directly.
- **AVG → SUM**: Replaces AVG with SUM and tags with `AVG_REWRITE_TAG` for execution-time scaling. The physical layer scales coefficients by the relevant row count, including WHEN/PER and aggregate-local WHEN masks. Objective AVG terms keep the tag so mixed expressions like `AVG(a) + SUM(b)` preserve true AVG semantics.
- **`<>` disjunction**: Creates BOOL indicator variables for not-equal comparisons.

This separation keeps semantic validation in the binder and algebraic transformation in the optimizer.

**Cross-reference**: See `../../03_expressivity/sql_functions/done.md` for detailed descriptions of each rewrite, including easy/hard case classification for MIN/MAX.
