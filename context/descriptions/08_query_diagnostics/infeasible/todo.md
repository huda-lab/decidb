# Query Diagnostics — Infeasible (planned)

The flagship state. The feasible region is empty; today both backends throw a
static paragraph (`gurobi_solver.cpp:229`, `deterministic_naive.cpp:208`). On
opt-in we build and solve a **second** optimization — the elastic program — whose
optimum *is* the least-change fix.

## Checklist

- [ ] **I1 · Elastic engine core** (v2.1) — deps: shipped foundations (F1/F2/F3)
- [ ] **I2 · Per-constraint-type elastic treatment** — deps: I1
- [ ] **I3 · L0 / removal dial** — deps: I1, norms (v1.1)
- [ ] **I4 · Infeasibility reporting** (v3.1) — deps: I1/I2, F4, F5

---

## I1 · Elastic engine core

**Goal.** Build the elastic program: add a non-negative slack to each relaxable
(PARAMETER / user) row, minimize a weighted L1 norm of the slack vector; the
support `{i : s*ᵢ > 0}` names which constraints to loosen and by how much.

```
min  Σ wᵢ sᵢ
s.t. Aᵢ x ≤ bᵢ + sᵢ ,  sᵢ ≥ 0      (user / PARAMETER rows only)
```

- **L1 default** — concentrates violation on a few constraints (sparse,
  interpretable). L0 is combinatorial (I3); L∞ spreads thinly.
- **Lexicographic two-stage:** (1) min slack → edit amounts; (2) freeze those
  amounts as bounds and re-solve the user's *original* objective over the
  now-feasible region → the returned solution + the **achievable objective** we
  report. Slack vars stay REAL even when RHS is integer.
- Slack on **user rows only** — F3 enforces; structural rows rigid.
- Built in our own model builder (not Gurobi `feasRelax`) so HiGHS runs it
  natively — solver-agnostic.
- **Scope signal:** if the elastic program itself is infeasible, the conflict
  reaches rigid structural rows → *"can't be fixed by loosening user
  constraints."* Surface distinctly.
- 🔬 **Confidence check (not a gate):** empirical guilt-subset check under the
  L1+count mix; weight-perturbation robustness (re-solve with perturbed `wᵢ`,
  intersect supports).

**Test (differential vs `oracle_solver`).** Known-infeasible inputs become
feasible with minimal slack; the returned solution matches an independent
re-solve over the relaxed region; structural rows never slackened; out-of-scope
conflicts flag elastic-infeasible.

**Why no shadow prices here:** shadow prices don't exist for ILPs (integrality
breaks duality) and there's no feasible baseline anyway; exact IIS is NP-hard. We
report exact slack amounts + achievable objective, no per-constraint duals.

---

## I2 · Per-constraint-type elastic treatment

**Goal.** Each constraint shape fans into matrix rows differently, so the slack
must attach correctly per shape. The full forward → slack-unit → reverse mapping
(with `λ` / `δ`) is in research note 8; port it here at build time. Key cases:

- **Shared slack across linearization blocks** (MIN/MAX/ABS/`=`): one editable
  knob `K` → **one shared slack column** across the block's N rows, not
  one-per-row. *Correctness* where rows diverge (easy `MAX(e) ≤ K`: the true edit
  is the `max` overshoot, not `Σ` of per-row overshoots — independent slacks
  misstate the amount ≈N× and make the global L1 race relax the wrong clause) and
  *actionability* where they don't. **This is the single biggest reason we
  hand-roll the engine** — `feasRelax` is one-slack-per-row and can't share.
- **PER:** one slack **per group** (the diagnosis granularity — which group, how
  much); the editable literal is shared across groups, so the runnable *edit* is
  one move `K → K + max_g s*_g`.
- **Per-row vs data column** (`x op col`): RHS is data, **no literal** — per-row
  slacks are the only honest output, rolled up to a conflict summary; no single
  query knob.
- **AVG** (`λ = N_g`), **strict `</>`** (carry `δ`), **quadratic** (slack on
  linear RHS only, never `Q`).
- **`<>`** → **remove-only** (excludes a single integer point; "loosen by x" is
  undefined) — gate with a removal binary (I3).
- **McCormick** → no own parameter, all rows rigid; the `U` bound is
  widen-not-remove.

**Test.** **Shared-slack divergence test:** easy `MAX(e) ≤ K` over several
violated rows must report the `max` overshoot (one edit), not `Σ`, and the engine
must pick the same clause to relax as the oracle. AVG reports in AVG units; `<>`
reports "had to be dropped," never "loosen by x."

---

## I3 · L0 / removal dial

**Goal.** The "remove a constraint" dial — uncapped slack gated by a binary `zᵢ`,
penalize `Σ zᵢ`. Mixing L1 + count means the engine prefers a small loosening and
removes only when loosening can't fix it. The removal set `{i : zᵢ = 1}` is the
minimum-cardinality hitting set — the closest thing to IIS diagnosis without
computing IISs. Required for `<>` (remove-only).

**Test.** Pure-loosenable cases use no removal; a remove-only `<>` conflict is
reported as a drop; mixed cases prefer loosening.

**Deps:** I1; reuses the count-binary+Big-M machinery from norms (v1.1).

---

## I4 · Infeasibility reporting (v3.1)

**Goal.** Render the slack solution at the user-clause level.

> **Schema note.** The shared `decide_diagnostics()` relation is now cross-state
> EAV: `(diagnosis_id, state, subject_kind, subject, attribute, value)`. Infeasible
> should render clause-level facts as rows such as `subject_kind='clause'`,
> `subject=<clause id/group>`, `attribute='edit_kind'/'suggested_change'/...`.
> No schema redesign is needed before I4; choose the exact attribute vocabulary in
> the engine. See `foundations/done.md`.

- **Always:** a structured edit list in EAV form — clause/group subject plus
  `edit_kind`, `suggested_change`, and related attributes; PER reported per group;
  plus the **achievable objective** from stage-2.
- **Conditionally:** a runnable rewritten DECIDE query — only when the edits
  collapse to one coherent clause; otherwise say why a single rewrite isn't
  expressible (PER groups sharing one `K`).
- **Honest wording:** positive-slack rows are *"involved in the conflict"*
  (proven: `s*ᵢ > 0 ⇒ i ∈ some IIS`); removal rows *"had to be dropped"* (weaker,
  always true). Never *"these are all the conflicts"* — the slacks give one
  hitting set, not the full IIS collection.

**Test.** End-to-end on constructed infeasible queries; PER divergence correctly
suppresses the single-query rewrite.

**Deps:** I1/I2, F4, F5.
