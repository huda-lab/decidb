# Stage 05 — Optimizer: open work

## Infeasible diagnostics: atomically drop NORM-L0 and IN formulations

**Pointers**: `src/decidb/utility/decide_diagnostic_engines.cpp` and
`src/include/duckdb/decidb/ilp_model.hpp`.

An `IN (...)` restriction and an L0 `norm(...)` expand to a group of cardinality,
indicator, and linking rows. Infeasibility repair must never loosen or independently
drop one of those rows: that would describe neither the original SQL clause nor a sound
relaxation of it.

**Required behavior**: record each formulation as one source-level, drop-only repair
group. A diagnosis may propose one `DROP <original SQL clause>` edit for the entire
group; it must not propose `LOOSEN`, a partial drop, or an internal indicator equation.

The existing `<>` removal path is specialized and cannot be reused as-is. Add a general
grouped-removal contract with a verified safe neutralization strategy; do not use an
arbitrary Big-M.

**Test**: infeasible IN and L0 queries report one source-SQL DROP action, and prove all
generated rows disappear together on both solver backends.
