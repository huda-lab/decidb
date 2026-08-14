# Stage 07 — Solver: open work

---

## No general-constraint / indicator / SOS channel exists

**Pointers**: `src/decidb/` has zero hits for `genconstr`, `SOS`, or the Gurobi
indicator APIs. Big-M is the only tool available anywhere in the codebase.

Gurobi models `z = max(...)` / `z = min(...)` natively via
`GRBaddgenconstrMax` / `GRBaddgenconstrMin`, and `GRBaddgenconstrIndicator` would
serve the `<>` disjunctions and ABS linearization. HiGHS has no equivalent.

**Decision**: this must be built as an **accelerator with the existing Big-M path
as fallback** — the pattern CLAUDE.md prescribes ("Gurobi-only APIs are
accelerators, never dependencies") — not as a replacement. Both backends stay
valid implementations.

**Why it matters**: the hard-direction MIN/MAX encoding is the one place where the
relaxation demonstrably weakens with row count, and it is what caps Q9's
benchmark scale. The measurement and the encoding analysis are recorded in
[`../../06_issues/code_quality/todo.md`](../../06_issues/code_quality/todo.md);
that entry is the authority on the *why*, this one on the backend channel it
needs.

**Test**: Q9 at 5K / 7.5K / 15K / 30K against the current Big-M curve, on both
backends — HiGHS must be unchanged, Gurobi must flatten.

**Done file**: `done.md` §4 — add the general-constraint path to the Gurobi table
and state the fallback rule.
