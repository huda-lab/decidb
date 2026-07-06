# Matrix Efficiency — Todo

Optimizations that make the ILP matrix smaller, tighter, or safer to solve.

---

No active tasks.

Constraint-to-bound conversion shipped — single-variable `x OP const` / `BETWEEN`
constraints are absorbed into column bounds (`TraverseBoundsConstraints`), see
`done.md` → "Constraint-to-Bound Absorption". Remaining structural ideas
(constraint push-down / row pruning) live in `../rewrite_passes/todo.md` and
`../future_work/todo.md`, gated on profiling that shows matrix assembly — not
solve time — is the bottleneck.
