# Query Diagnostics — Foundations (remaining)

Shared infrastructure consumed by all diagnosis states. **Build first** — every
state engine depends on these. This file tracks remaining foundation work; landed
foundation notes live in `done.md`.

## Landed (see `done.md`)

- **F2** · Constraint provenance · **F4** · `PRAGMA diagnose_decide` · **F5** ·
  `decide_diagnostics()` relation · **F6** · Variable provenance (column-side;
  index→name + aux→expression, consumed by the unbounded diagnosis to name escaping
  vars). Detailed notes in `done.md`.

## Remaining

- [ ] **F3 · Relaxability tagging** (v1.2-B) — deps: F2 (satisfied; the `kind` field
  + USER/global-STRUCTURAL split landed with F2 — F3 owes the *exhaustive* STRUCTURAL
  stamping across the linearization paths). Detail below.

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
