# Query Diagnostics — Infeasible (planned)

The flagship state. The feasible region is empty. The elastic engine — a **second**
optimization whose optimum identifies which user constraints to loosen and by how much —
is fully shipped (I1–I5; see `done.md`). The remaining work is the two deferred shapes
below: uncorrelated scalar-subquery RHS (I2) and aggregate `<>` removal (I4).

> **Router terminal:** `failed → infeasible` → `elastic` → report (`router/README.md`).
> The terminal classifies distinctly in `RouteSolveResult` (`decide_router.cpp`) and
> the `DiagnosisTerminal::INFEASIBLE` arm in `physical_decide.cpp` invokes
> `DiagnoseInfeasible` when `PRAGMA diagnose_decide` arms diagnostics.

Phase tags (`v2.1`, `v3.1`) trace to the research build plan; they encode build
order, not versions.

## Checklist

- [x] **I1 · Elastic model + stage-1 solve, simple shapes** (v2.1) — shipped; see `done.md`
- [x] **I2 · Slack placement — shared-slack / multi-row shapes** — shipped; see `done.md`
  ("per-shape slack placement"). All of I2.0/a/b/c/d/e landed.
- [x] **I3 · Stage-2 — freeze-budget objective re-solve** — shipped; see `done.md`
  ("stage-2 achievable objective (freeze-budget)").
- [x] **I4 · L0 / removal dial** — shipped (per-row `<>`); see `done.md`
  ("L0 / removal dial"). Aggregate `<>` removal deferred (see below).
- [x] **I5 · Infeasibility reporting (full)** (v3.1) — shipped; see `done.md`
  ("Infeasibility reporting: lean cue summary + frozen vocabulary"). The runnable-rewritten-query
  ambition was deliberately dropped (no source text; table is source of truth).

---

## Known I2 limitation · uncorrelated scalar-subquery RHS

**Current behavior.** A per-row bound whose RHS is an uncorrelated scalar subquery
(`x <= (SELECT 5)`) is enforced correctly by the main solver, but infeasible diagnosis
treats the subquery RHS as per-row data. The resulting diagnosis is therefore a data
conflict summary, not an editable shared cap that loosens to a single value. The
foldable-expression sibling (`x <= 2 + 3`) is fixed and now behaves like a shared literal
cap.

**Reproduction.**

```sql
SELECT id, x
FROM (VALUES (1,10)) t(id, lo)
DECIDE x IS REAL
SUCH THAT x <= (SELECT 5) AND x >= lo
MAXIMIZE SUM(x);
```

Under `PRAGMA diagnose_decide='auto'`, this reports a data-RHS conflict summary
(currently the competing floor `x >= 10 conflicts in 1 of 1 rows`, depending on the
solver's tie choice) rather than `Loosen x <= 5 to x <= 10`.

**Why this remains.** Shared-literal classification currently uses
`Expression::IsFoldable()` in the `physical_decide.cpp` constraint RHS-evaluation block.
That is false for a subquery. By the time the physical operator runs, the optimizer has
flattened `(SELECT 5)` into a join, so the RHS arrives as a column reference and is not
structurally distinguishable from genuine row data without correlation/plan analysis.
Blindly tagging it as shared would be wrong for correlated subqueries, whose RHS is
legitimately row-varying.

**Future fix.** Detect row-invariant flattened subquery RHS values by structure
(uncorrelated by construction, not merely constant by data coincidence) and classify them
as `SHARED_LITERAL` or a new shared-scalar shape, while correlated subqueries remain
`PER_ROW_DATA`.


---

## I4 follow-up · aggregate `<>` removal

I4 shipped the removal dial for **per-row `<>`** (see `done.md`, "L0 / removal dial"):
weighted single solve (B1, `DIAGNOSTIC_REMOVAL_WEIGHT` above the slack weights), one binary
`w` per `<>` wired `±M₂` into its disjunction pair, grouped by the new
`ConstraintProvenance::indicator_col`, reported as an `edit_kind='drop'` EAV row, with the
`diagnose_decide_removal_bigm` pragma exposing M₂ (auto default).

**Remaining:** aggregate `<>` (`SUM(x) <> K`). Its disjunction binary is a **global-block
aux column** with no user-facing label channel (`BuildColumnProvenance` leaves global aux
columns unlabeled), so a dropped aggregate `<>` cannot be named. The mechanic would work, but
the DROP edit's subject would be empty. The fix is a label channel for global-var indicators
(record the clause text at the aggregate-`<>` site in `physical_decide.cpp` and surface it
through column provenance), then tag the aggregate rows' `indicator_col` (the
`SolverInput::RawConstraint` → global-site-4 propagation, reverted in I4 to avoid nameless
drops). Until then aggregate `<>` keeps its prior static-error behavior.

---

## Suggested batches

- **Batch A (the thin slice):** I1 — simple-shape stage-1 + elastic-infeasible signal +
  minimal edit list, on the shipped I0 seam. **Shipped.**
- **Batch C (the hard shapes):** I2 — shared-slack placement + per-shape units; the bulk of
  the engine. **Shipped.**
- **Batch B (objective):** I3 — freeze-budget stage-2 (shape-agnostic). **Shipped.**
- **Batch D (reporting):** I5 — lean cue summary + frozen `edit_kind` vocabulary. **Shipped.**
- **Batch E (gated):** I4 — L0 / removal dial. **Shipped** for per-row `<>` (norms landed,
  unblocking it); aggregate `<>` removal remains (see "I4 follow-up").

---

## Notes to revisit

- **Slack weights are uniform among editable knobs (`wᵢ = 1`); data-RHS slacks are
  penalized.** I1 shipped with uniform weights (decision 2); I2.c added a higher weight on
  `PER_ROW_DATA` slacks (`DIAGNOSTIC_DATA_SLACK_WEIGHT`) so editable constraints loosen first.
  Among editable knobs the weight is still uniform, so one unit of loosening costs the same
  regardless of scale. Revisit if a scale-mixed oracle case misbehaves — with mixed units the
  L1 race can prefer loosening the large-scale constraint. The fix is scale-normalized weights
  (by RHS magnitude / row-coefficient norm); deferred until a test exposes the skew.
- **Weighted preference ladder is a fixed-constant stand-in, not lexicographic.** Two coarse
  weights encode preferences in the single stage-1 objective: `DIAGNOSTIC_DATA_SLACK_WEIGHT`
  (`1e3`, prefer editable edits over data conflicts, I2.c) and `DIAGNOSTIC_REMOVAL_WEIGHT`
  (`1e6`, prefer any loosening over dropping a `<>`, I4) — giving the ladder editable `1` <
  data `1e3` < removal `1e6`. I3 did **not** replace them: the stage-2 budget freeze picks the
  objective-best fix *among min-loosening edits* (a tier below S*), which is orthogonal to the
  preferences that live *inside* the stage-1 weights. A true lexicographic ladder ("editable,
  then data, then removal" — drop the weights, run stage 1 in successive passes) remains the
  proper fix for **both** weights at once — still future work. Revisit if a scale-mixed oracle
  case makes a weight misorder the fixes.
