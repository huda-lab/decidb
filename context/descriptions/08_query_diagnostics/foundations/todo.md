# Query Diagnostics — Foundations (remaining)

Shared infrastructure consumed by all diagnosis states. **Build first** — every
state engine depends on these. This file tracks remaining foundation work; landed
foundation notes live in `done.md`.

## Checklist

- [x] **F2 · Constraint provenance** (v1.2-A) — DONE (see `done.md`)
- [ ] **F3 · Relaxability tagging** (v1.2-B) — deps: F2 (satisfied; the `kind` field
  + USER/global-STRUCTURAL split landed with F2 — F3 owes the *exhaustive* STRUCTURAL
  stamping across the linearization paths)
- [x] **F4 · Invocation pragma `PRAGMA diagnose_decide`** — DONE (see `done.md`)
- [x] **F5 · Diagnostic reporting relation** — DONE (see `done.md`, surfaced as the
  `decide_diagnostics()` table function)
- [ ] **F6 · Variable provenance** (column-side; index→name + aux→expression) — deps: none; used by U3

External dependency (tracked in `03_expressivity/sql_functions/todo.md`):
**decision-variable norms (v1.1)** — abs-aux / count-binary+Big-M / max-aux
linearizations reused by the elastic engine (`infeasible/` I3).

---

## F3 · Relaxability tagging (Pillar B)

**Goal.** Distinguish *user* rows (choices — relaxable) from *structural* rows
(definitions — rigid). Slackening a McCormick / Big-M row redefines the math and
solves a different problem, so the elastic engine must slacken only user rows.

**Build.**
- Stamp `kind`: `USER` on the plain emission path, `STRUCTURAL` in each
  linearization expansion path. Stamped, not inferred — structural rows already
  emit in separate builder paths keyed on explicit tags.
- **Row-role** the elastic engine needs (research note 8): within a clause each
  row is PARAMETER (carries the user's editable `K`; slack lands here) or
  MECHANISM (linking `Σy≥1`, McCormick/ABS definitions; rigid). Open design call:
  second field, or a refinement of `kind`?
- Variable bounds: the only structural case is the McCormick-required finite UB —
  **widenable but not removable** (widening keeps it finite/safe; removal breaks
  the envelope). Gate the removal dial, not the widen dial.
- **Enumerate every structural kind** and confirm each maps to a distinct
  emission path: McCormick, Big-M MIN/MAX, `<>`, ABS, AVG scaling, entity-scoping
  links, composed-MIN/MAX pins. Structural rewrites live in
  `src/optimizer/decide/decide_optimizer.cpp`.

**Test.** Every structural rewrite stamps STRUCTURAL; the elastic program never
slackens a structural row; an all-structural conflict makes the elastic program
itself infeasible (the scope diagnostic, not a fake fix).

**Deps:** F2.

> The `kind` field + the USER / global-STRUCTURAL split already landed with F2.
> F3's remaining work is the **exhaustive** STRUCTURAL stamping across the
> linearization rewrite paths (and the PARAMETER/MECHANISM row-role call).

The slack→Δ conversion the elastic engine consumes (AVG `s*/N_g`, strict `</>`
re-quote against the typed `K`) was scoped to F5 originally but **deferred to the
infeasible engine** — unbounded produces no slacks, so it had no live consumer.
Build it here (or in `infeasible/`) when the elastic engine needs it; render it
through the existing `decide_diagnostics()` relation (F5, done).

---

## F6 · Variable provenance (column-side)

**Goal.** Map every solver *column* back to the user-facing thing it represents,
so the unbounded diagnosis can name the escaping variable (U3). The column-side
complement of F2 (which does rows/clauses).

Today names die at the solver boundary (P7): `VarIndexer` (`ilp_model.hpp:23-75`)
maps `(decide_var_idx, row)` → flat column index, but no names reach `SolverInput`
/ `SolverModel`. User names live only in `LogicalDecide.decide_variables[*].alias`
(`logical_decide.hpp:51`). Auxiliary columns from linearization (`__abs_aux_N__`,
`__bilinear_aux_N__`, `__minmax_ind_N__`, `__ne_ind_N__`) carry generated names +
partial link metadata, but **no link to the source expression**.

**Build (full — per the U3 "full output" decision).**
- **User variables:** thread the `.alias` from `LogicalDecide` through to the
  solver result so a column index resolves to the user's variable name. Modest.
- **Auxiliary variables:** capture an aux→source-expression link at optimizer
  time (where the expressions still exist — the ABS / MIN-MAX / McCormick / `<>`
  passes in `src/optimizer/decide/decide_optimizer.cpp`), so an escaping aux
  resolves to the user's original `ABS(...)` / `MAX(...)` / product expression,
  not the internal name. ~300–400 lines across LogicalDecide + execution layer
  (P7). **This is the bulk of the "full output" cost.**
- Reverse map: flat column index → `(decide var | aux)` → name / expression.

**Test.** A column index resolves to the correct user variable name; an auxiliary
column resolves to its originating user expression across ABS / MIN-MAX /
McCormick / `<>`.

**Deps:** none. **Used by:** U3 (ray→SQL naming). Column-side complement of F2.
