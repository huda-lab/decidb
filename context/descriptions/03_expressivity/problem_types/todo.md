# Problem Types — Planned Features

## SOCP (Second-Order Cone Programming)

**Priority: Low**

SOCP is a natural generalization beyond DeciDB's current QCQP support. It would
enable constraints of the form `||Ax + b|| <= c^T x + d`.

A practical use case would be robust optimization or true Euclidean
norm-bounded constraints:

```text
||new_val - target||_2 <= radius
```

**Design note**: Gurobi supports second-order and rotated cone constraints
through convex quadratic constraints. HiGHS currently exposes LP, MIP, and QP
models, while DeciDB's HiGHS backend rejects quadratic constraints. A
solver-agnostic implementation therefore needs either a proven reformulation
using HiGHS-supported primitives or an additional backend capability; Gurobi
support alone is not a shared DeciDB contract. Surface syntax must also be
distinguished from the existing `norm(e, 2)` desugaring, which represents
squared L2 rather than the square-root Euclidean norm.

---
