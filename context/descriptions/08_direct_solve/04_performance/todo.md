# Performance — open work

Measure only after the [correctness gate](../03_correctness/todo.md) passes.
The existing pipeline profiling area is
[`05_performance/`](../../05_performance/README.md); direct-solve reports should
use compatible workload, backend, and readback boundaries.

- [ ] **PERF-01 — End-to-end phase accounting.** Depends: VAL-05. Separate
  direct analysis, plan construction, relational execution, and result
  readback. Compare against solver model construction, backend loading, solve,
  and readback on both supported backends. Include total wall time.
- [ ] **PERF-02 — Scale and memory sweep.** Depends: PERF-01. Vary input rows,
  capacity, sign mix, and tie density. Record peak memory, intermediate
  cardinality, and planning overhead. Keep source query, configuration, and raw
  measurements reproducible.
- [ ] **PERF-03 — Selection policy evidence.** Depends: PERF-02. Determine when
  direct is materially better and when it is not. Do not enable automatic
  selection by default merely because one case wins. A cost model is warranted
  only when correct alternatives genuinely compete.

**Exit gate:** retain/promote the rule only with a complete benefit report that
does not count timeouts, solver limits, unsupported cases, or memory skips as
successful comparisons.
