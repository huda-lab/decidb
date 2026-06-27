# Query Diagnostics — Foundations (remaining)

Shared infrastructure consumed by all diagnosis states. The structured result,
constraint/variable provenance, relaxability tagging, the pragma gate, and the
reporting relation have shipped (`done.md`). What remains:

## Slack → Δ conversion (build when the elastic engine needs it)

The slack→Δ conversion the elastic engine consumes (AVG `s*/N_g`, strict `</>`
re-quote against the typed `K`) has no live consumer yet — unbounded produces no
slacks. Build it here or in `infeasible/` when the elastic engine lands; render it
through the existing `decide_diagnostics()` relation.

## External dependency

**Decision-variable norms (v1.1)** — abs-aux / count-binary+Big-M / max-aux
linearizations reused by the elastic engine (`infeasible/` I4). Tracked in
`03_expressivity/sql_functions/todo.md`.
