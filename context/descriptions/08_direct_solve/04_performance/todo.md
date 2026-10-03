# Performance — open work

No performance task is open for S1. Measured results are in [done.md](done.md), the
[benefit report](s1_benefit_report.md), the [large-scale report](s1_large_scale.md), and the
[API phase report](s1_api_phase.md). Raw runs are in [raw/](raw/). The existing pipeline
profiling area is [`05_performance/`](../../05_performance/README.md); new direct-solve
reports use compatible workload, backend, and readback boundaries.

A new promotion (a wider S1 shape or another rule) adds its own tasks here. Each needs its
own end-to-end measurement that does not count timeouts, solver limits, unsupported cases,
or memory skips as successful comparisons. Cost-based choice between direct plans is
[NEXT-05](../05_follow_on/todo.md).
