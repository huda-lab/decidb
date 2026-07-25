# Code Quality Issues — Open

Code-quality issues (duplication, dead code, fragile patterns, unclear naming, missing test coverage) spotted opportunistically while working on other tasks. Not bugs — nothing here produces wrong results today; these are things that make the code harder to change safely. Actual bugs go to `../bugs/todo.md`.

Each entry: short title, location (`file:line`), what's wrong, why it matters, and when/during which task it was discovered.

Resolved entries are removed; if the fix taught a generalizable lesson, record it in `.claude/lessons.md`.

---

## `total_constraints` benchmark metric mixes two units

**Location**: `src/execution/operator/decide/physical_decide.cpp:5974`

```cpp
size_t bench_total_constraints = solver_input.constraints.size() + solver_input.global_constraints.size();
```

The two summands are not the same unit. `solver_input.constraints` holds `EvaluatedConstraint` **specs** — a per-row spec expands to one model row per data row (`ilp_model_builder.cpp:714-720`), and a PER spec expands to one row per group. `solver_input.global_constraints` holds already-expanded **rows**. Adding them reports a number that is neither spec count nor matrix row count.

**Why it matters**: the metric is the only model-size signal in the benchmark output (`benchmark/decide/view_results.py:175`, `bigm_experiment.py:95`), so it silently understates the models where per-row expansion dominates — exactly the cases worth optimizing. Q4 large reports 1,000,007 constraints; the real matrix is roughly 2M rows, because the easy-direction `MAX(keep * l_quantity) <= 45` rewrite contributes one row per data row while counting as a single spec. Anyone reading the benchmark to decide where the row-count pressure is gets pointed at the wrong clause.

**Fix direction**: report the post-`Build` count (`model.constraints.size()`, plus `model.quadratic_constraints.size()`) instead of summing pre-expansion inputs — that is the number the solver actually receives. Emitting both (specs vs. expanded rows) as separate fields would keep the clause-level view too, but requires a matching `view_results.py` change.

**Discovered**: 2026-07-26, while investigating why Q4 (`minmax_nested`) dominates wall-clock in the `fa156b6a6d` benchmark run.

---

## Hard-direction MIN/MAX has only a one-hot Big-M encoding, whose relaxation weakens with row count

**Location**: `src/execution/operator/decide/physical_decide.cpp:5394-5432` (flat hard MIN/MAX objective); the same encoding appears for composed terms via `EmitComposedHardMinMaxIndicators` and for nested-PER inner/outer levels.

The hard direction (`MAXIMIZE MAX`, `MINIMIZE MIN`) emits the textbook one-hot Big-M encoding: one indicator binary per active row, a linking row `z <= v_r + M*y_r` per row, and `SUM(y) >= n-1` so exactly one row binds.

The Big-M constant is not the weakness — `compute_big_m()` (line 4925) returns `global_max - global_min`, and since `z`'s own bound *is* the global extreme, a per-row constant would not be tighter. The **encoding** is the weakness: setting every `y_r = (n-1)/n` is LP-feasible and slackens the bound on `z` by `M*(n-1)/n`, which tends to `M` as `n` grows. The root relaxation therefore gets weaker the larger the instance, and branch-and-bound must close a gap that widens with row count.

**Why it matters**: this makes Q9 the least scalable query in the benchmark suite — measured 2026-07-26 at 5K 1.7s, 7.5K 5.3s, 15K 29.8s, 30K >60s, against a near-linear curve for every other MILP in the set. That ranking is an artifact of the single formulation we implement, not evidence that hard-MAX is intrinsically harder than the rest. It also caps `Q9_ROW_LIMIT` at 7.5K/15K while comparable queries run at 500K/1M.

**Fix direction**: DeciDB currently has no general-constraint, indicator-constraint, or SOS path at all — `src/decidb/` has zero hits for `genconstr`, `SOS`, or the indicator APIs, so Big-M is the only tool available anywhere in the codebase. Gurobi's `GRBaddgenconstrMax` / `GRBaddgenconstrMin` model `z = max/min(...)` natively and avoid the relaxation problem entirely. HiGHS has no equivalent, so this must be built as an **accelerator with the existing Big-M path as fallback** — the same pattern CLAUDE.md prescribes for diagnostics ("Gurobi-only APIs are accelerators, never dependencies"), not as a replacement. Introducing the general-constraint channel would also open `GRBaddgenconstrIndicator` for the `<>` disjunctions and ABS linearization, which use Big-M for the same reason.

**Discovered**: 2026-07-26, while raising benchmark scale limits and asking which limits are inherent problem complexity versus our formulation. Ruled out as *not* the cause in the sibling case: Q3's L0 Big-M looseness (`norm(adj, 0, 40)` against a tight bound of 20) measured identical to the tight and inferred variants at both 60K and 120K, so Gurobi presolve repairs a loose user-supplied constant. Big-M *tightness* is handled; Big-M *encoding* is not.
