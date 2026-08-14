# Stage 05 — Optimizer: open work

---

## Infeasible diagnostics: atomically drop NORM-L0 and IN formulations

**Pointers**: `src/decidb/utility/decide_diagnostic_engines.cpp` and
`src/include/duckdb/decidb/ilp_model.hpp`.

An `IN (...)` restriction and an L0 `norm(...)` expand to a group of cardinality,
indicator, and linking rows. Infeasibility repair must never loosen or
independently drop one of those rows: that would describe neither the original
SQL clause nor a sound relaxation of it.

**Required behavior**: record each formulation as one source-level,
**drop-only** repair group. A diagnosis may propose one `DROP <original SQL
clause>` edit for the entire group; it must not propose `LOOSEN`, a partial drop,
or an internal indicator equation.

**Design constraint**: the existing `<>` removal path is specialized and cannot
be reused as-is. Add a general grouped-removal contract with a verified safe
neutralization strategy before exposing this repair; do not use an arbitrary
Big-M.

**Test**: infeasible IN and L0 queries report one source-SQL DROP action, and
prove all generated rows disappear together on both solver backends.

---

## No cost-based backend or formulation selection

**Pointers**: `SelectSolverBackend()` in `src/decidb/utility/ilp_solver.cpp`;
`is_easy` computation in `src/optimizer/decide/decide_optimizer.cpp`.

Two selections are made statically today:

1. **Backend** — Gurobi whenever available, regardless of problem
   characteristics. That is defensible (Gurobi is faster on every measured
   workload) but it is not a decision, it is a default.
2. **MIN/MAX formulation** — easy vs hard follows purely from direction and the
   factor's sign. It does not consider row count, even though the hard encoding's
   relaxation is known to weaken as rows grow (see
   [`../07_solver/todo.md`](../07_solver/todo.md)).

**Decision needed**: whether formulation choice should consult
`estimated_cardinality`. It is available on the operator, and the hard-MIN/MAX
weakness is a function of row count specifically — but a cost model that is wrong
is worse than a static choice that is predictable, and DeciDB has no calibration
data for one.

**Test**: whatever rule is chosen must not change any of the 80 golden models
below the threshold it introduces.

**Done file**: `done.md` §2 (MIN/MAX) and `../07_solver/done.md` §2 (backend).
