# Stage 05 — Optimizer: open work

---

## The header's pass inventory is out of date

**Pointers**: `src/include/duckdb/optimizer/decide_optimizer.hpp:24-35`.

The "Current passes" comment lists four; there are seven, plus
`TagAbsConstraintsForBigM`. Missing: `RewriteComposedMinMax`,
`RewriteComposedMinMaxObjectiveTop`, `RewriteBilinear`.

The "Future passes (to be migrated from binder)" list is also wrong. `IN` domain
rewrite still lives in `bind_select_node.cpp` and is filed against stage 01, not
here. "Partition-solve detection" and "variable bound propagation" do not exist in
any form and have no design behind them — if they are still wanted they need a
real entry, and if not the lines should go.

**Decision**: whether the two speculative future passes are dropped or written up.
Dropping is the honest default; a one-line aspiration in a header is not a plan.

**Test**: n/a — comment only.

**Done file**: `done.md` §1 already carries the real inventory; this just makes the
header agree with it.

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
