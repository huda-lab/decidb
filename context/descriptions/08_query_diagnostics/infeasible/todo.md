# Query Diagnostics — Infeasible (planned)

The flagship state. The feasible region is empty. I1/I2 now diagnose the shipped
least-change shapes by building and solving a **second** optimization — the *elastic
program* — whose optimum identifies which user constraints to loosen, and by how much.
The remaining work deepens that result with objective-aware tie-breaking, removal, and
full reporting.

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
- [ ] **I3 · Stage-2 — freeze-budget objective re-solve** — deps: I1 (shape-agnostic)
- [ ] **I4 · L0 / removal dial** — deps: I1, norms (v1.1, external)
- [ ] **I5 · Infeasibility reporting (full)** (v3.1) — deps: I1/I2/I3, F4, F5

---

## Known I2 limitation · uncorrelated scalar-subquery RHS

**Current behavior.** A per-row bound whose RHS is an uncorrelated scalar subquery
(`x <= (SELECT 5)`) is enforced correctly by the main solver, but infeasible diagnosis
treats it as a per-row *data* conflict (`x <= 5 conflicts in N of N rows`) instead of an
editable shared cap that loosens to a single value. The foldable-expression sibling
(`x <= 2 + 3`) is fixed and now behaves like a shared literal cap.

**Reproduction.**

```sql
SELECT id, x
FROM (VALUES (1,10)) t(id, lo)
DECIDE x IS REAL
SUCH THAT x <= (SELECT 5) AND x >= lo
MAXIMIZE SUM(x);
```

Under `PRAGMA diagnose_decide='auto'`, this reports a conflict summary on `x <= 5`
rather than `Loosen x <= 5 to x <= 10`.

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

## I3 · Stage-2 — freeze-budget objective re-solve

**Goal.** Report the **achievable objective** the user gets after the minimal fix — and
the specific edit that achieves it. Shape-agnostic: works on whatever slacks I1/I2 placed.

**Method (freeze the budget, not the amounts).** Keep the slacks as **variables**, cap the
total loosening at the stage-1 optimum `S*`, and re-solve the user's original objective:

```
stage 2:   max  cᵀx          (the user's original objective)
           s.t. Aᵢ x ≤ bᵢ + sᵢ
                Σ wᵢ sᵢ ≤ S*(1 + ε)         ← freeze the budget; s stays variable
                sᵢ ≥ 0 , structural rows rigid
           → report the stage-2 slacks as the edit, and cᵀx* as the achievable objective
```

This is a lexicographic objective: *(1)* minimize total loosening → `S*` (I1), *(2)* among
**all** min-loosening edits, pick the one that maximizes the user's objective. It strictly
dominates freezing the exact amounts, which locks in an *arbitrary* stage-1 minimizer when
the min-slack solution isn't unique. **Report the stage-2 slacks** (not stage-1's) as the
edit so the edit and the objective are consistent.

**Decisions to settle (I3):**
- **Budget tolerance `ε`.** Absolute (`Σwᵢsᵢ ≤ S* + ε`) vs. relative (`S*(1+ε)`). Needed
  because `S*` is a solved value — too tight spuriously makes stage-2 infeasible. Pick
  against the backend feasibility tolerance; reuse `DIAGNOSTIC_RAY_EPSILON` /
  `diagnostic_constants.hpp` style constants or add one.
- **Stage-2 unbounded.** If the relaxed region is unbounded in the objective direction,
  stage-2 has no finite max. Decide: report the edit + "objective is unbounded after this
  fix," or hand to the unbounded engine. Settle the message.
- **Sparsity tie-break (defer).** Among edits tying on objective, the solver picks one
  arbitrarily — possibly denser than L1's sparse pick. If edits look dense in practice,
  add a 3rd lexicographic tier (re-minimize spread/count). Default: skip for v1.
- **Surface `S*`?** Whether total loosening `S*` is reported as its own fact alongside the
  per-edit amounts.

**Test (differential vs `oracle_solver`).** On a constructed non-unique-minimizer case
(e.g. two relaxable caps feeding one structural floor), stage-2 must report a *better*
objective than freezing the exact amounts would, and the reported edit must be the one
that achieves the reported objective when re-solved independently.

**Done section:** "Elastic engine: stage-2 achievable objective (freeze-budget)."

---

## I4 · L0 / removal dial

**Goal.** The "remove a constraint" dial — uncapped slack gated by a binary `zᵢ`, penalize
`Σ zᵢ`. Mixing L1 + count means the engine prefers a small loosening and removes only when
loosening can't fix it. The removal set `{i : zᵢ = 1}` is the minimum-cardinality hitting
set — the closest thing to IIS diagnosis without computing IISs. Required for `<>`
(remove-only, I2).

**Decisions to settle (I4):**
- **Penalty mixing.** Pure lexicographic (min loosening, then min removals) vs. weighted
  sum (`Σ wᵢ sᵢ + M · Σ zᵢ`). Settle the priority / `M` and how it composes with the I3
  budget freeze.
- **Offered vs forced removal.** `<>` is remove-only (forced); all other shapes get
  removal only when loosening cannot restore feasibility.

**Test.** Pure-loosenable cases use no removal; a remove-only `<>` conflict is reported as
a drop; mixed cases prefer loosening.

**Deps:** I1; reuses the count-binary + Big-M machinery from **norms (v1.1)** — an external
dependency tracked in `03_expressivity/sql_functions/todo.md`. Blocked until that lands.

**Done section:** "Elastic engine: L0 / removal dial."

---

## I5 · Infeasibility reporting (full) (v3.1)

**Goal.** Render the slack solution at the user-clause level through `decide_diagnostics()`.
I1 ships a minimal edit list; this completes it.

> **Schema.** `decide_diagnostics()` is cross-state EAV
> `(diagnosis_id, state, subject_kind, subject, attribute, value)` (`decide_diagnostic.hpp`,
> impl `decide_diagnostic.cpp`, registered `system_functions.cpp`). Infeasible renders
> clause-level facts as rows like `subject_kind='clause'`, `subject=<clause id / group>`,
> `attribute='edit_kind' | 'suggested_change' | 'amount' | ...`. No schema redesign needed.

- **Always:** a structured edit list in EAV form — clause/group subject plus `edit_kind`,
  `suggested_change`, amount, and related attributes; PER reported per group; plus the
  **achievable objective** from I3.
- **Conditionally:** a runnable rewritten DECIDE query — only when the edits collapse to
  one coherent clause; otherwise say why a single rewrite isn't expressible (e.g. PER
  groups sharing one `K`).
- **Honest wording** (per the user-facing-output principle — actionable, no solver jargon):
  positive-slack rows are *"involved in the conflict"* (proven: `s*ᵢ > 0 ⇒ i ∈ some IIS`);
  removal rows *"had to be dropped"* (weaker, always true). Never *"these are all the
  conflicts"* — the slacks give one hitting set, not the full IIS collection.

**Decisions to settle (I5):**
- **Attribute vocabulary.** The exact `attribute` strings the infeasible engine emits
  (`edit_kind`, `suggested_change`, `amount`, `group`, `achievable_objective`,
  `elastic_infeasible`, …) and their `value` formats. Keep them stable once chosen.
- **Rewrite trigger + render.** The precise rule for "edits collapse to one clause" → emit
  a runnable query, and the suppression message otherwise.

**Test.** End-to-end on constructed infeasible queries (both backends,
`test/decide/tests/test_query_diagnostics_relation.py`); PER divergence correctly
suppresses the single-query rewrite; the elastic-infeasible case renders its distinct row.

**Deps:** I1/I2/I3, F4, F5.

---

## Suggested batches

- **Batch A (the thin slice):** I1 — simple-shape stage-1 + elastic-infeasible signal +
  minimal edit list, on the shipped I0 seam. **Shipped.**
- **Batch C (the hard shapes):** I2 — shared-slack placement + per-shape units; the bulk of
  the engine. **Shipped.**
- **Batch B (objective):** I3 — freeze-budget stage-2 (shape-agnostic). *Next.*
- **Batch D (reporting):** I5 — full two-tier reporting once shapes + objective exist.
- **Batch E (gated):** I4 — L0 / removal dial, when norms (v1.1) land.

---

## Notes to revisit

- **Slack weights are uniform among editable knobs (`wᵢ = 1`); data-RHS slacks are
  penalized.** I1 shipped with uniform weights (decision 2); I2.c added a higher weight on
  `PER_ROW_DATA` slacks (`DIAGNOSTIC_DATA_SLACK_WEIGHT`) so editable constraints loosen first.
  Among editable knobs the weight is still uniform, so one unit of loosening costs the same
  regardless of scale. Revisit if a scale-mixed oracle case misbehaves — with mixed units the
  L1 race can prefer loosening the large-scale constraint. The fix is scale-normalized weights
  (by RHS magnitude / row-coefficient norm); deferred until a test exposes the skew.
- **Data-slack penalty is a fixed constant, not lexicographic.** `DIAGNOSTIC_DATA_SLACK_WEIGHT`
  is a coarse stand-in for preferring editable edits; a true lexicographic "editable first, then
  data" tier is the proper fix and folds into the **I3** stage-2 work.
