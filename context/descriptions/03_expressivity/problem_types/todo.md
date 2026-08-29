# Problem Types — Planned Features

## SOCP (Second-Order Cone Programming)

**Status: Deferred — not planned for the current release.**

SOCP is a natural generalization beyond DeciDB's current QCQP support. It would
enable constraints of the form `||Ax + b|| <= c^T x + d` — true Euclidean
norm-bounded constraints, and robust optimization built on them:

```text
||new_val - target||_2 <= radius
```

**Why deferred.** The practical case is already reachable. A radius bound
`||e|| <= R` and its squared form `||e||^2 <= R^2` have the same solution set,
and the squared form is expressible today: `norm(e, 2)` desugars to
`SUM(POWER(e, 2))`, which lowers to a quadratic constraint. What SOCP adds over
that is the square-root spelling itself, plus the non-convex `>=` direction,
which has to be refused either way. Neither earns a new problem class.

**What a future implementation must settle.**

- *Surface syntax.* `norm(e, 2)` already means **squared** L2. A square-root
  Euclidean norm needs a distinct spelling, and the two living side by side is a
  standing hazard for users — resolving that is the main design cost here.
- *Direction.* `||e|| <= b` is convex and solvable; `||e|| >= b` is not, and must
  be refused at bind time with a message naming the clause.
- *Backend reach.* Gurobi expresses second-order and rotated cones through convex
  quadratic constraints; HiGHS exposes LP, MIP and QP only. A Gurobi-only class is
  consistent with how DeciDB already ships quadratic constraints, non-convex QP
  and MIQP: add a flag to `SolverModelClass`
  (`src/include/duckdb/decidb/solver/solver_capabilities.hpp`) and let
  `RequireDecideSolverSupport` refuse at plan time with a named gap. Waiting on a
  HiGHS-expressible reformulation is not required by any existing contract.

Revisit if a concrete query turns up that the squared form cannot express.
