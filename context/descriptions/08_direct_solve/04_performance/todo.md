# Performance — open work

Measured results are in [done.md](done.md), the [large-scale report](s1_large_scale.md),
and the [API phase report](s1_api_phase.md). Raw runs are in [raw/](raw/). The
existing pipeline profiling area is [`05_performance/`](../../05_performance/README.md);
new direct-solve reports use compatible workload, backend, and readback boundaries.

Each task below can be picked up on its own. When a task is done, its result moves
to [done.md](done.md) and the task is deleted from this file.

- [ ] **PERF-01 — Separate relational execution from in-engine result collection.**
  - Goal: report relational execution wall time and collector append, combine, and
    finalize time as two separate numbers for the wide full-output case.
  - Depends: none. The correctness gate (VAL-05) is done.
  - Evidence and code: [collector phase check](s1_api_phase.md#collector-phase-check-on-full-output),
    [materialized vs streamed output](s1_api_phase.md#materialized-and-streamed-full-output),
    runner `benchmark/decide/profile_direct_s1_api.cpp`.
  - Done when: five-million-row wide full-output runs on both solver backends give
    execution time and collection time separately, the method of separating them is
    written down, and it does not change result semantics. Streaming is an
    end-to-end comparison, not an exact subtraction, so it does not satisfy this task.
  - Moves to: [done.md](done.md).
- [ ] **PERF-02 — Query-only peak memory for wide rows and aggregate-local `WHEN`.**
  - Goal: measure peak memory of the query alone, without source-table creation,
    for the 512-byte-payload source and for the aggregate-local `WHEN` shapes.
  - Depends: PERF-01 (same runner and sources, so do them together).
  - Evidence and code: [window sort-buffer snapshot](s1_api_phase.md#window-sort-storage-and-joined-source-pruning),
    [aggregate-local `WHEN`](s1_api_phase.md#aggregate-local-when-at-scale),
    [source-valued local `WHEN`](s1_api_phase.md#source-valued-aggregate-local-when-at-scale).
    The fixed-pin, source-valued bound, paired-bound, and `COALESCE` sweeps also have
    no query-only peak memory; include them if the same runner covers them.
  - Done when: direct and Gurobi peak memory for those shapes are reported on the
    same five-million-row sources with setup excluded, and the report says whether the
    wide direct plan is above or below Gurobi. The sort-buffer counter is not a peak.
  - Moves to: [done.md](done.md).
- [ ] **PERF-03 — Benefit report and selection policy.**
  - Goal: say when direct is materially better and when it is not, then decide what
    `auto` does.
  - Depends: PERF-01 and PERF-02.
  - Evidence and code: [large-scale report](s1_large_scale.md) (wide and
    zero-capacity counterexamples), [API phase report](s1_api_phase.md) (readback can
    erase a direct gain), [setting `decide_direct_solve`](../README.md).
  - Done when: one report lists the shapes where direct wins and where it does not,
    counts no timeout, solver limit, unsupported case, or memory skip as a successful
    comparison, and records the policy in [decisions.md](../00_design/decisions.md).
    The default stays `off` unless that report supports changing it.
  - Moves to: [done.md](done.md). Cost-based choice between direct plans is
    [NEXT-05](../05_follow_on/todo.md), not part of this task.

## Suggested batches

| Batch | Tasks | Why together |
| --- | --- | --- |
| A | PERF-01, PERF-02 | One runner session over the same five-million-row sources |
| B | PERF-03 | Needs the numbers from batch A |

**Exit gate:** retain or promote the rule only with a complete benefit report that
does not count timeouts, solver limits, unsupported cases, or memory skips as
successful comparisons.
