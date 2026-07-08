# Problem Types — Planned Features

---


## Negative Variable Domains

**Finite negative domains already work; only the fully-free (-∞) domain is a real gap.**

The earlier premise here — "all variables have a non-negative lower bound of 0, so negative values can't be expressed" — is outdated. A finite negative domain is expressible today with an explicit lower bound, which constraint absorption folds into an O(1) column bound (no matrix row, no `x_pos - x_neg` split):

```sql
DECIDE x IS REAL    SUCH THAT x >= -10 AND x <= 10   -- domain [-10, 10], works
DECIDE d IS INTEGER SUCH THAT d >= -5                -- domain [-5, +inf), works
```

Signed variables with finite negative bounds shipped (see `03_expressivity/decide/done.md`); the absorption is documented in `04_optimizer/matrix_efficiency/done.md` → "Constraint-to-Bound Absorption".

The one genuine remaining gap is the **fully-free `(-∞, +∞)` domain** (a truly unbounded-below variable, e.g. an opt-in `IS REAL UNBOUNDED` / `FREE`). This is **deliberately deferred by design** — an unbounded-below variable is the case most likely to make objectives unbounded — and is tracked in `03_expressivity/decide/todo.md` (it also gates the `-∞` escape branch of the unbounded diagnostics, `08_query_diagnostics/unbounded/todo.md`). Not restated as an active task here.

---

## Explicit Variable Bound Syntax (`IN [a, b]`) — sugar only, deprioritized

`DECIDE x IS INTEGER IN [0, 100]` would be pure **syntactic sugar** over `SUCH THAT x >= 0 AND x <= 100`. Both the clarity-vs and the original efficiency argument ("bounds are O(1), constraint rows are not") are now moot: constraint absorption already turns `x >= a AND x <= b` into the exact same O(1) column bounds (`04_optimizer/matrix_efficiency/done.md`), and every domain the syntax could express — including negative ones (above) — already works via constraints. So this buys only conciseness, at the cost of a checked-in generated-parser regen.

Left here only as an optional ergonomic nicety, low priority. Not planned. If revisited, it should reuse the existing bound arrays (`absorbed_*_bounds`), not add a parallel bound path.

---


## SOCP (Second-Order Cone Programming)

**Priority: Low**

SOCP is a natural generalization beyond QP. Both Gurobi and HiGHS support second-order cone constraints. This would enable constraints of the form `||Ax + b|| <= c^T x + d`.

A practical use case would be robust optimization or norm-bounded constraints:

```sql
-- NOT YET SUPPORTED — hypothetical syntax
SUCH THAT NORM(new_val - target) <= budget
```

**Design note**: SOCP sits between QP and SDP in the optimization hierarchy. Gurobi supports it via `GRBaddqconstr` (rotated SOC) or `GRBaddgenconstrNorm`. HiGHS has experimental QP support but SOC support may be limited. This is a significant architectural addition that should be evaluated after QCQP.

---