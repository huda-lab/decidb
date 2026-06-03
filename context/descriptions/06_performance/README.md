# Performance Optimization Log

Append-only record of performance optimizations applied to DeciDB. Each entry describes the change set, the hypothesis, and the measured outcome on the standard `benchmark/decide` suite.

The intent is to keep an honest, dated trail so future work can:
- See what has already been tried (and not re-do it)
- Compare proposed changes against the prevailing baseline
- Track which structural areas (model building, coefficient evaluation, solver handoff, etc.) have already had their easy wins extracted

## Convention

- One file per optimization batch, named `{NNN}_{baseline_commit}_{evaluated_commit}.md` where `NNN` is a zero-padded sequential log number, `baseline_commit` is the most recent commit *before* the change, and `evaluated_commit` is the commit being evaluated. Example: `002_9c3a53fb62_6bc8ae1412.md`.
- Each file states: **what changed**, **why** (the hypothesis), **how it was measured**, and **the outcome** (per-query deltas vs. the prior commit).
- Reference both the commit hash that introduced the change and the benchmark JSON files (`benchmark/decide/results/<commit>.json`) used for the comparison.
- Don't rewrite past entries. If a later change supersedes an earlier one, add a new entry that says so.

## Entries

- [001](001_d534f8b8ed_9c3a53fb62.md) — Model-building speed quartet. Bound absorption, row-scoped fast path, vector reservations, tautology dropping, deferred objective copy. Commit `9c3a53fb62` vs baseline `d534f8b8ed`.
- [002](002_9c3a53fb62_6bc8ae1412.md) — Batched ExpressionExecutor, vectorized DOUBLE extract, typed hash keys for entity / PER grouping. Commit `6bc8ae1412` vs baseline `9c3a53fb62`. Total wall −8.4%; Q7 entity-scope-join −55%, Q2 −40%, Q3/Q4 −33%.
