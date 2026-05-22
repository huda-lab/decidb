# Problem Types — Planned Features

---

## ~~Feasibility Problems (No Objective)~~

**Done** — see [done.md](done.md). The grammar now accepts `DECIDE ... SUCH THAT ...` without `MAXIMIZE`/`MINIMIZE`. Both Gurobi and HiGHS support feasibility problems natively.

---

## Negative Variable Domains (Unrestricted Variables)

**Priority: Medium**

Currently all variable types have a non-negative default lower bound (0). This prevents expressing problems where variables naturally range over negative values (e.g., regression coefficients, profit/loss deltas, temperature deviations).

```sql
-- NOT YET SUPPORTED
DECIDE x IS REAL UNRESTRICTED          -- domain: (-inf, +inf)
DECIDE delta IS INTEGER UNRESTRICTED   -- domain: (..., -1, 0, 1, ...)
```

**Workaround**: Split a variable into positive and negative parts (`x_pos - x_neg`) but this doubles the variable count and requires manual constraint bookkeeping.

**Design note**: Requires changes to default bounds in `ilp_model_builder.cpp` (lower bound `-1e30` instead of `0`) and a new grammar token for the `UNRESTRICTED` modifier. Solver backends already support unrestricted variables natively.

---

## Explicit Variable Bound Syntax

**Priority: Medium**

Currently variable bounds come from type defaults and simple constraint extraction (the model builder intersects explicit bounds from constraints like `x >= 5`). An explicit bound syntax would be clearer and more efficient — bounds are O(1) per variable in the solver, while constraint rows are not.

```sql
-- NOT YET SUPPORTED
DECIDE x IS INTEGER IN [0, 100]
DECIDE y IS REAL IN [-10, 10]
```

**Design note**: This overlaps with "negative variable domains" — explicit bounds would subsume UNRESTRICTED as a special case of `IN [-inf, +inf]`. See also `04_optimizer/matrix_efficiency/` for the bound-extraction optimization that partially addresses this from the optimizer side.

---

## ~~QCQP (Quadratically Constrained Quadratic Programming)~~

**Done** — see [done.md](done.md). `POWER(linear_expr, 2)` is now supported in `SUCH THAT` constraints (Gurobi only; HiGHS rejects with a clear error). Leverages the existing `GRBaddqconstr` infrastructure from the bilinear constraint implementation.

## ~~Products of Decision Variables (Bilinear Terms)~~

**Done** — see [bilinear/done.md](../bilinear/done.md). Bilinear terms (`x * y`) are supported in both objectives and constraints. Boolean × anything is linearized via McCormick envelopes (both solvers). General non-convex bilinear uses Q matrix / `GRBaddqconstr` (Gurobi only).

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

## SDP Boundary Note

Semi-definite programming (SDP) is **not planned**. Neither Gurobi nor HiGHS is an SDP solver. This is noted here as an explicit boundary of DecidB's intended expressiveness — DecidB targets the LP/ILP/MILP/QP/MIQP family, not the broader conic programming hierarchy.
