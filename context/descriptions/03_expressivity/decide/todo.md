# DECIDE Clause — Planned Features

---

## EXPLAIN renders a qualified reducer identically to an unqualified one

`EXPLAIN` prints `sum((opening_cost * open))` for both `SUM(D: opening_cost * open)` and
`SUM(opening_cost * open)`, so the plan gives no way to tell two queries with different
optima apart. The tag itself does not leak into the output (it lives on the aggregate's
`alias`, which `ToString()` does not print) — the qualifier is simply not rendered.

Unlike `WHEN` and `PER`, which `CollectTaggedExpressionStrings`
(`src/planner/operator/logical_decide.cpp`) unwraps into postfix suffixes, the qualifier
sits *on* the aggregate node rather than above it, so surfacing it means intercepting the
aggregate's own rendering rather than appending a suffix. Cosmetic, but it costs a reader
the one detail that distinguishes the two plans.

*Discovered 2026-08-08 while shipping the single-relation qualifier.*
