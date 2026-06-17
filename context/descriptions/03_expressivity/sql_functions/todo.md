# SQL Functions & Expressions — Planned Features

---

## Decision-Variable Norms (L0 / L1 / L2 / L∞)

**Implemented** as the `norm(expr, p)` function — see `done.md` ("norm(expr, p)
— L_p Regularization") and `../../00_project_overview/syntax_reference.md`.
Desugared at bind time: p = 1 → `SUM(ABS)`, p = 2 → `SUM(POWER(·,2))`,
p = 'inf' → `MAX(ABS)`, p = 0 (count) → indicator + Big-M `ABS(e) <= M*z`, term
→ `SUM(z)`. The user supplies the weight λ directly.

### Remaining work

- **Scale-free α / smart-λ.** Auto-selecting the weight via `α = λ/λ_max`
  (validated externally: L0/L1 have a crisp finite `λ_max`, ridge/L2 saturates
  asymptotically) was **intentionally deferred** — judged overcomplicated for SQL
  users and it needs a multi-solve. Revisit only if requested.

---

## Division (`/`) Over Decision Variables

**Not planned**. Division by a decision variable is inherently non-linear. Division by a constant is valid but can be handled by multiplying the other side (already possible with current syntax).

---

## NOT Over Decision Variable Expressions

**Not planned**. `NOT` applied to a decision variable expression would require a binary negation auxiliary variable. Use `x = 0` or `1 - x` instead.

---

## IN on Aggregates

**Not planned**. `SUM(x) IN (...)` is not supported. Use multiple equality constraints or BETWEEN instead.
