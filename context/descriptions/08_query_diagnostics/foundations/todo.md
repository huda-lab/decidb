# Query Diagnostics — Foundations (remaining)

Shared infrastructure consumed by all diagnosis states. The structured result,
constraint/variable provenance, relaxability tagging, the pragma gate, and the
reporting relation have shipped (`done.md`). What remains:

## Slack → Δ conversion — home settled: `infeasible/` (I2.d)

**Settled:** the slack→Δ conversion lands **with the engine in `infeasible/`** (I2.d), since
the infeasible engine is its only consumer (unbounded produces no slacks). Per-shape units:
AVG reports the **raw slack** (row coeffs are pre-scaled by `1/N_g`, so it is already in AVG
units — *supersedes* the earlier `s*/N_g` note); strict `<` / `>` re-quotes against the typed
`K` via the `strict` / `typed_k` provenance fields. Rendered through the existing
`decide_diagnostics()` relation. See `infeasible/todo.md` I2.d.

## External dependency

**Decision-variable norms (v1.1)** — abs-aux / count-binary+Big-M / max-aux
linearizations reused by the elastic engine (`infeasible/` I4). Tracked in
`03_expressivity/sql_functions/todo.md`.
