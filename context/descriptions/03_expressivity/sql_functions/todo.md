# SQL Functions & Expressions — Planned Features

---

## Decision-Variable Norms (L0 / L1 / L2 / L∞)

**Priority: Medium — planned.** Foundation feature (research phase v1.1). The
query-diagnostics elastic engine reuses its linearization machinery — see
`../../08_query_diagnostics/infeasible/todo.md` (I3) and that area's `README.md`.

**Syntax: undesigned.** There is no `NORM` / `L1` / `L0` surface today; designing
it is the first open item, and the canonical spec
(`../../00_project_overview/syntax_reference.md`) must be updated before this doc.
This is distinct from the hypothetical norm-bounded *constraint* `NORM(...) <= budget`
floated in `../problem_types/todo.md` — that bounds a norm inside `SUCH THAT`; this
exposes a norm as an *objective* over decision variables.

### Why it's needed — the diffuse-answer failure mode

A query can solve fine yet return a useless answer. "Minimize magnitude"
objectives (e.g. `MINIMIZE SUM(ABS(new − old))`) admit many equally-optimal
solutions, and the solver returns an arbitrary **diffuse** one — a thousand tiny
changes instead of a few legible ones. The solve succeeds; the *answer* is the
failure. The fix is **expressivity**, not an automatic rewrite: expose norms as a
first-class, user-selected construct so the user states the intent. DeciDB never
silently reformulates the user's objective.

### The four norms and their linearizations

| Norm           | Meaning                                       | Linearization                              |
| -------------- | --------------------------------------------- | ------------------------------------------ |
| **L0 / count** | number of changed variables (minimal *count*) | per-variable binary + finite bound, Big-M  |
| **L1**         | sum of magnitudes (sparse-ish, linear)        | abs-aux (reuses ABS — `done.md`)           |
| **L2**         | Euclidean                                     | convex QP — already in the solver layer    |
| **L∞**         | max single deviation                          | max-aux (reuses MAX — `done.md`)           |

Most of the machinery already exists: ABS Path A/B (abs-aux) and easy/hard
MIN/MAX (max-aux) are documented in `done.md`, and L2 is supported through the
existing QP path. The genuinely new work is the **L0 count** construct
(per-variable indicator binary + finite bound + Big-M) and the user-facing syntax
to select a norm.

### Open questions

- **Syntax** (above) — function-like `NORM(expr, p)`, or per-norm keywords?
  Update the spec first.
- **Which norms ship first** — L0 + L1 are the high-value pair.
- **L0's per-variable finite bound** interacts with structural variable bounds (it
  needs a finite Big-M per variable).

### Testing

Differential vs `oracle_solver` on constructed cases (never hand-computed): L0
yields minimal-*count* edits; L1 / L∞ match the oracle's norm-minimizing point.

---

## Division (`/`) Over Decision Variables

**Not planned**. Division by a decision variable is inherently non-linear. Division by a constant is valid but can be handled by multiplying the other side (already possible with current syntax).

---

## NOT Over Decision Variable Expressions

**Not planned**. `NOT` applied to a decision variable expression would require a binary negation auxiliary variable. Use `x = 0` or `1 - x` instead.

---

## IN on Aggregates

**Not planned**. `SUM(x) IN (...)` is not supported. Use multiple equality constraints or BETWEEN instead.
