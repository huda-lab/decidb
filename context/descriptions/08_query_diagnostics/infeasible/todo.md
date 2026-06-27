# Query Diagnostics — Infeasible (planned)

The flagship state. The feasible region is empty; today the solve returns
`INFEASIBLE` and `PhysicalDecide::Finalize` routes it to the static error
(`ThrowDecideSolveError`, `ilp_solver.cpp:133`). On a failed solve under `auto` we
instead build and solve a **second** optimization — the *elastic program* — whose
optimum *is* the least-change fix: which user constraints to loosen, and by how much.

> **Router terminal:** `failed → infeasible` → `elastic` → report (`router/README.md`).
> The terminal already classifies distinctly in `RouteSolveResult`
> (`decide_router.cpp:17`); the `DiagnosisTerminal::INFEASIBLE` arm in
> `physical_decide.cpp:5419` currently throws the static error and is where the
> engine drops in (router R5).

**I0 (engine seam + INFEASIBLE routing) has shipped** — see `done.md` ("Engine seam:
infeasible"). Build the rest in order: **I1 → I2 → I3 → I4 → I5**. Each phase is
independently pickable and carries its pointers, the **decisions to settle at
implementation time**, how to test, and which `done.md` section to write when it lands.
I3 (stage-2) is shape-agnostic and *may* land right after I1 if you want the simple-
shape slice complete end-to-end before tackling shared-slack shapes.

Phase tags (`v2.1`, `v3.1`) trace to the research build plan; they encode build
order, not versions.

## Checklist

- [x] **I1 · Elastic model + stage-1 solve, simple shapes** (v2.1) — shipped; see `done.md`
- [ ] **I2 · Slack placement — shared-slack / multi-row shapes** — deps: I1
- [ ] **I3 · Stage-2 — freeze-budget objective re-solve** — deps: I1 (shape-agnostic)
- [ ] **I4 · L0 / removal dial** — deps: I1, norms (v1.1, external)
- [ ] **I5 · Infeasibility reporting (full)** (v3.1) — deps: I1/I2/I3, F4, F5

---

## I1 · Elastic model + stage-1 solve, simple shapes (v2.1)

**Goal.** The first end-to-end vertical slice. Build the elastic program for *simple*
relaxable constraints, solve it (stage 1), read which constraints to loosen and by how
much, and emit a minimal edit list — proving the whole seam before the hard shapes.

```
stage 1:   min  Σ wᵢ sᵢ
           s.t. Aᵢ x ≤ bᵢ + sᵢ ,  sᵢ ≥ 0      (relaxable rows + relaxable bounds only)
                structural / mechanism rows rigid
           →  the support {i : s*ᵢ > 0} names the edits; s*ᵢ is the amount
```

- **Simple shapes only:** plain linear `expr op K` rows tagged `USER_PARAMETER`
  (`IsRelaxableForElastic`, `decide.hpp:41`), and ABS `|e| ≤ K` (reduces to a single
  `USER_PARAMETER` pin row; its envelope rows are `STRUCTURAL` and stay rigid,
  `physical_decide.cpp:4043,4057`). Shared-slack / multi-row shapes are **I2**.
- **L1 default** — concentrates violation on a few constraints (sparse, interpretable).
  L0 is combinatorial (I4); L∞ spreads thinly.
- **Slack on relaxable rows only** — filter by `provenance.kind == USER_PARAMETER`
  using `BuildClauseRowIndex` (`ilp_model.hpp:184`, impl `ilp_model_builder.cpp:994`).
  `USER_MECHANISM` / `STRUCTURAL` rows are rigid.
- **Built in our own model builder** (extend `SolverModel::Build` / a transform over the
  built model), **not** Gurobi `feasRelax`, so HiGHS runs it natively — solver-agnostic.
- **Elastic-infeasible scope signal.** If the elastic program *itself* is infeasible,
  the conflict reaches rigid structural rows → *"can't be fixed by loosening your
  constraints."* Surface as a distinct outcome (its own EAV row / message), not a
  generic failure.

**Decisions to settle (I1):**
- **★ Relaxable bounds, not just rows (load-bearing).** A user constraint `x ≤ 10` /
  `x BETWEEN a AND b` is **absorbed into the column-bound arrays** and never emitted as
  a row (`physical_decide.cpp:1248` pre-absorb, `TraverseBoundsConstraints` `:1268`,
  `absorbed_bound_exprs` `:2136`, copied to `solver_input.lower_bounds/upper_bounds`
  `:3530-3541`). So it carries **no provenance** and is invisible to `BuildClauseRowIndex`
  — a row-only elastic model **cannot loosen it**, yet bounds (`x ≤ capacity`,
  `0 ≤ x ≤ 1`) are the most common constraint. **Decide:** *(a)* suppress bound
  absorption under diagnosis and rebuild the elastic model with user bounds as
  slackable rows (recommended — uniform, gets provenance for free, off the hot path
  since it's a second solve), or *(b)* tag user-derived column bounds and add slack to
  the bound in place. Either way you must distinguish a *user* `x ≤ 10` from the
  default non-negativity (`lower = 0`) and BOOLEAN `0/1` bounds, which stay rigid —
  `absorbed_bound_exprs` holds the source-expression pointers and is the hook.
- **Slack weights `wᵢ`.** Uniform (`wᵢ = 1`) vs. scale-normalized (by RHS magnitude /
  row-coefficient norm). Start uniform; note the skew risk — with mixed units the L1
  race prefers loosening the large-scale constraint. Settle when a scale-mixed oracle
  case misbehaves.
- **Constraint sense / slack direction.** `≤` → `b + s`; `≥` → `b − s`; `=` needs both
  directions. Decide the `=` representation: one *signed* slack vs. two non-negative
  slacks. (`=` is also a shared-slack block in I2 when it fans into multiple rows.)
- **Slack variable type.** REAL, `sᵢ ≥ 0`, uncapped (capping/gating is the L0 dial, I4)
  — **even when the RHS is integer** (an integer RHS does not force an integer edit).

**Test (differential vs `oracle_solver`).** Known-infeasible simple-shape inputs become
feasible with minimal slack; the support + amounts match an independent re-solve over the
relaxed region; structural/mechanism rows never slackened; a conflict that needs a rigid
row flags **elastic-infeasible**. Include at least one absorbed-bound case to lock the
bounds decision.

**Why no shadow prices:** shadow prices don't exist for ILPs (integrality breaks
duality), there's no feasible baseline, and exact IIS is NP-hard. We report exact slack
amounts (+ the achievable objective from I3), never per-constraint duals.

**Done section:** "Elastic engine: stage-1 core (simple shapes) + elastic-infeasible signal."

---

## I2 · Slack placement — shared-slack / multi-row shapes

**Goal.** Attach slack correctly where one editable knob fans into several matrix rows.
The forward → slack-unit → reverse mapping (with `λ` / `δ`) is in research note 8; port it
here. **This is the single biggest reason we hand-roll the engine** — `feasRelax` is
one-slack-per-row and cannot share. Each shape's correctness is proven by a stage-1
solve (I1 machinery) + oracle diff.

- **Shared slack across a linearization block** (MIN/MAX/ABS/`=`): one editable knob `K`
  → **one shared slack column** wired into all N rows of the block, not one-per-row. The
  easy `MAX(e) ≤ K` case strips to N per-row `USER_PARAMETER` rows that all share
  `clause_id` and carry `was_minmax_easy` (`physical_decide.cpp:1437`): the true edit is
  the **max overshoot** (one shared slack), and independent per-row slacks misstate it
  ≈N× and make the global L1 race relax the wrong clause.
- **PER:** one slack **per group** (`by_clause_group`, keyed `(clause_id, group_key)`);
  the editable literal is shared across groups, so the runnable *edit* is one move
  `K → K + max_g s*_g`.
- **Per-row vs data column** (`x op col`): RHS is data, **no literal** — per-row slacks
  are the only honest output, rolled up to a conflict summary; no single query knob.
- **AVG** (`λ = N_g`: report `s*/N_g`), **strict `</>`** (carry `δ`, re-quote against the
  typed `K`), **quadratic** (slack on the **linear RHS only**, never `Q` — quadratic
  rows carry provenance too, `ilp_model.hpp:154`).
- **`<>`** → **remove-only** (excludes a single integer point; "loosen by x" is
  undefined) — gated by the removal binary in **I4**; mechanism rows are `USER_MECHANISM`
  (`physical_decide.cpp:3646`).
- **McCormick** → no own parameter, all rows rigid (`STRUCTURAL`, `:3890-3938`); the `U`
  bound is widen-not-remove.

**Decisions to settle (I2):**
- **Block-identification rule per shape.** Group by `clause_id` (easy-MAX, ABS) vs.
  `(clause_id, group_key)` (PER) — settle which key each shape uses, and how to detect a
  shared-slack block vs. genuinely independent rows that happen to share a clause.
- **Shared-slack column wiring.** Confirm the model builder lets one slack column appear
  in N constraint rows with the correct sign; decide whether to add it during
  `SolverModel::Build` or in the elastic transform.
- **Slack → Δ conversion home + units.** Currently has no live consumer
  (`foundations/todo.md`). Decide whether it lands in `foundations/` or here, and pin the
  per-shape unit map (AVG `s*/N_g`, strict re-quote with `δ`, MIN/MAX shared-overshoot).

**Test.** **Shared-slack divergence test:** an easy `MAX(e) ≤ K` violated on several rows
must report the `max` overshoot (one edit), not `Σ`, and pick the same clause to relax as
the oracle. AVG reports in AVG units; `<>` reports "had to be dropped," never "loosen by x";
quadratic slackens only the linear RHS.

**Done section:** "Elastic engine: per-shape slack placement (shared blocks, PER, AVG, strict, quadratic)."

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
  minimal edit list, on the shipped I0 seam. First user-visible infeasible diagnosis.
- **Batch B (objective):** I3 — freeze-budget stage-2 on the simple shapes (shape-agnostic,
  can precede I2).
- **Batch C (the hard shapes):** I2 — shared-slack placement; the bulk of the engine.
- **Batch D (reporting):** I5 — full two-tier reporting once shapes + objective exist.
- **Batch E (gated):** I4 — L0 / removal dial, when norms (v1.1) land.

---

## Notes to revisit

- **Slack weights are uniform (`wᵢ = 1`).** I1 shipped with uniform weights (decision 2),
  so one unit of loosening costs the same on every constraint regardless of its scale.
  Revisit if a scale-mixed oracle case misbehaves — with mixed units the L1 race can prefer
  loosening the large-scale constraint. The fix is scale-normalized weights (by RHS
  magnitude / row-coefficient norm); deferred until a test exposes the skew.
