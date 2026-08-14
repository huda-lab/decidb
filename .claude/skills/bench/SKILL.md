---
name: bench
description: Automates the DeciDB optimization loop - build, benchmark, view results, analyze stage timings, and suggest the next optimization. Use when the user wants to run benchmarks, measure performance, or find the next perf improvement.
---

# /bench — Build, Benchmark, Analyze

Automates the DeciDB optimization loop: build → benchmark → view → analyze → suggest.

## Arguments

`$ARGUMENTS` — space-separated, combinable:
- *(empty)* — build if needed, benchmark `--sizes medium`, auto-compare
- `full` — all sizes (medium, large)
- `medium`, `large`, or `medium,large` — specific sizes
- `manual` — run `queries/manual.sql`
- `compare` — explicitly force comparison with previous commit
- `skip-build` — skip `make release`

## Procedure

### 1. Parse arguments

Parse `$ARGUMENTS` into:
- **sizes**: default `medium`. If `full`, use `medium,large`. If explicit sizes given, use those.
- **do_build**: default `true`. Set `false` if `skip-build` is present.
- **do_compare**: default `true`. Forced `true` if `compare` is present.
- **is_manual**: `true` if `manual` is present.

### 2. Build (unless skip-build)

Check if `build/release/decidb` exists and is older than the newest file under `src/`. If stale or missing, run:
```bash
make release
```
**STOP on build failure** — do not proceed to benchmarking.

If `skip-build` was specified, skip this step entirely.

### 3. Ensure databases exist

Check if the required database files exist under `benchmark/decide/databases/` (e.g., `medium.db`). If any are missing, run:
```bash
make decide-bench-setup
```

### 4. Run benchmarks

Run with stage timers enabled:
```bash
DECIDB_BENCH=1 python3 benchmark/decide/run_benchmarks.py --sizes {sizes} [--compare] [--manual]
```

Add `--compare` if `do_compare` is true.
Add `--manual` if `is_manual` is true (omit `--sizes` for manual mode unless explicit sizes given).

### 5. View results

Run the visual viewer:
```bash
python3 benchmark/decide/view_results.py
```
(The benchmark runner already invokes this, but run it explicitly to capture the output for analysis.)

### 6. Analyze

Read the JSON result file from `benchmark/decide/results/` (the most recent one, or `manual.json` for manual mode).

For each query entry, compute:
- **Solver%**: `solver_ms / (wall_time_s * 1000) * 100`
- **Model%**: `model_construction_ms / (wall_time_s * 1000) * 100`
- **Optimizer%**: `optimizer_ms / (wall_time_s * 1000) * 100`

Present a summary table in this format:

```
## Benchmark Results (commit {hash}, {sizes})

| Query | Size  | Rows | Vars | Constraints | Wall(s) | Solver% | Model% | Opt% |
|-------|-------|------|------|-------------|---------|---------|--------|------|
| Q1    | medium| 60K  | 60K  | 3           | 0.45    | 93%     | 5%     | 1%   |
```

### 7. Suggest next optimization

Study the benchmark profile holistically — stage percentages, variable/constraint counts, scaling across sizes, regressions vs previous results — and use your judgment to suggest the most impactful next move.

Consider factors like:
- Where time is actually spent (solver, model construction, optimizer)
- Whether variable/constraint counts seem proportional to the problem or inflated by auxiliary rewrites
- Scaling behavior across sizes if multiple were run
- Regressions vs previous commits if comparison data is available
- What the query's DECIDE clause actually does (read the SQL) and whether the formulation could be tighter

Also check `context/descriptions/05_performance/` for already-applied optimizations so you don't re-suggest one.

Point to specific files and code paths in your suggestions. Be concrete and actionable, not generic.

For each suggestion, **explain and defend it**:
1. **What**: the specific optimization (e.g., "eliminate redundant indicator variables for simple MAX constraints")
2. **Why it matters**: tie it back to the benchmark data — show which numbers would change and why (e.g., "Q3 has 120K vars for 60K rows, meaning 60K auxiliaries from the Big-M rewrite. Eliminating them halves the solver input.")
3. **Mechanism**: explain *how* the optimization works at the ILP level — why it produces an equivalent but smaller/tighter formulation
4. **Where**: the exact code path to modify

Format the output as:

```
### Analysis
- [key observations from the data]

### Suggested Optimization
**What**: [the optimization]
**Why**: [which benchmark numbers motivate this, and the expected impact]
**Mechanism**: [how it works at the ILP/formulation level]
**Where**: [specific files and code paths]
```
