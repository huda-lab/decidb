# Query Diagnostics — Unbounded (remaining)

The objective improves without limit — the infeasible machine run backwards:
region too *open* in the improving direction. The *fix* is forced (the user must
add a bound or fix a sign error — you can't relax your way out), but the
*diagnosis* can be rich: name the exact variables escaping to infinity.

## Checklist

- [ ] **U3 · Ray→SQL mapping + reporting** (full output) — deps: U2 ✅, F2 ✅, F4 ✅,
  F5 ✅, **F6 (only open dep)**. The reporting stack + a scaffold diagnosis are wired
  end-to-end (see `done.md`); U3 swaps the scaffold for named escaping variables.

This file tracks remaining unbounded work; landed unbounded notes live in
`done.md`.

---

## U3 · Ray→SQL mapping + reporting (full output)

**Goal.** Turn the ray into a user-facing diagnosis.

- Collect variables with `dᵢ > 0`; map each to its SQL name via **F6 variable
  provenance**. **Decision: full output** — if a ray variable is an auxiliary
  (`MAX(expr)` z, ABS aux, McCormick / `<>`), trace it back to the user's original
  expression (F6's aux→expression link), not the internal `__abs_aux_N__` name.
- **DecidB narrows suspects for free (verified, P7):** only `IS INTEGER` /
  `IS REAL` can escape (BOOLEAN locked to [0,1]); all *user* vars are non-negative
  so escape is +∞; McCormick factors have a finite UB so can't be the source.
  Mechanical culprit test: MAXIMIZE → positive objective coefficient with no upper
  cap. The candidate filter needs **no new data** — it runs on the model's
  existing bounds / coeffs / types.
- **Report:** escaping variables (fully named), likely cause ("missing
  capacity/budget constraint or objective sign error"), prescription "add an
  upper bound." **Never pick the bound value** — any finite bound works; the right
  number is domain knowledge. Open: whether to *suggest* an example value (e.g.
  the largest RHS in scope) or stay silent to avoid a bad anchor.
- **Shared reporting surface (why F2 is a dep):** U3 emits through the unified
  **F5** relation, not a one-off variable-only format, so every diagnosis state
  renders consistently — and **F5 needs F2** for clause-level labels. Unbounded's
  *own* content is variable-centric (F6 names the escaping vars), so F2's primary
  payoff here is **building + battle-testing the shared F2+F5 reporting stack on
  the simpler unbounded case** before the infeasible engine relies on it.
  *Optional enrichment F2 enables (not v1):* a constraint-aware line — "clause N
  references escaping var x but never bounds it."

**Test (differential).** Correct escaping variables named — user vars AND aux vars
traced to their source expression; `INF_OR_UNBD` routes correctly.

**Deps:** U2 ✅, **F2 ✅** (constraint provenance — supplies F5's clause labels),
**F4 ✅** (consent gate), **F5 ✅** (`decide_diagnostics()` relation), and the one
remaining open dep **F6** (variable provenance — full, incl. aux→expression).
