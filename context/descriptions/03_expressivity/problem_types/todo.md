# Problem Types — Planned Features

---

## Explicit Variable Bound Syntax (`IN [a, b]`) — sugar only, deprioritized

`DECIDE x(INT) IN [0, 100]` would be pure **syntactic sugar** over `SUCH THAT x >= 0 AND x <= 100`. Both the clarity-vs and the original efficiency argument ("bounds are O(1), constraint rows are not") are now moot: constraint absorption already turns `x >= a AND x <= b` into the exact same O(1) column bounds (`01_pipeline/05_optimizer/done.md`), and every domain the syntax could express — including negative ones — already works via constraints. So this buys only conciseness, at the cost of a checked-in generated-parser regen.

Left here only as an optional ergonomic nicety, low priority. Not planned. If revisited, it should reuse the existing bound arrays (`absorbed_*_bounds`), not add a parallel bound path.

---

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
