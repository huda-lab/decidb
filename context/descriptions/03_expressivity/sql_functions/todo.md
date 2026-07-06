# SQL Functions & Expressions — Planned Features

---


## Division (`/`) Over Decision Variables

**Not planned**. Division by a decision variable is inherently non-linear. Division by a constant is valid but can be handled by multiplying the other side (already possible with current syntax).

---

## NOT Over Decision Variable Expressions

**Not planned**. `NOT` applied to a decision variable expression would require a binary negation auxiliary variable. Use `x = 0` or `1 - x` instead.

---

## IN on Aggregates

**Not planned**. `SUM(x) IN (...)` is not supported. Use multiple equality constraints or BETWEEN instead.
