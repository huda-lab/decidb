# Query Diagnostics — Slow (planned)

**Least-settled of the four states — by design.** "Slow" is four states wearing
one mask: infeasible-still-searching, unbounded-still-searching,
proving-optimality, or feasible-but-hard-to-find. The first action on a slow
solve (hitting `DECIDB_TIME_LIMIT` without a proven optimum) is *not* to relax —
it's to **read what the solver already knows** and route by the **best bound**.
Revisit this design after infeasible + unbounded ship; testing those builds the
intuition for what's actually slow.

> Gurobi reads `DECIDB_TIME_LIMIT` (300s default, `gurobi_solver.cpp:70-81`);
> HiGHS sets no time limit today (F1). No interrupt mechanism exists yet.

## Checklist

- [ ] **S1 · Interrupt infrastructure** — deps: F1
- [ ] **S2 · Read incumbent / bound / gap at interrupt** — deps: S1
- [ ] **S3 · Routing (Bucket A/B + unbounded hand-off)** (v3.2) — deps: S2, I1
- [ ] **S4 · Anytime objective→constraint switch** (Bucket A) — deps: S3

---

## S1 · Interrupt infrastructure

**Goal.** A mechanism to interrupt a running solve — none exists today.

- 🔬 **Design decision + probe:** signal / statement-timeout / solver callback?
  Which does each backend support?

**Deps:** F1.

---

## S2 · Read incumbent / bound / gap at interrupt

**Goal.** At interrupt, read the solver's current `incumbent`, `best_bound`,
`gap` on both backends — the discriminators for routing.

- 🔬 **Probe:** confirm HiGHS exposes incumbent / bound / gap on interrupt (else
  Bucket A is reduced on HiGHS).

**Deps:** S1, F1.

---

## S3 · Routing (v3.2)

**Goal.** Route by the best bound — the clean discriminator (an unbounded MILP
usually *has* incumbents but its bound runs to ∞):

| At interrupt                    | Diagnosis           | Action                              |
| ------------------------------- | ------------------- | ----------------------------------- |
| incumbent + **finite** bound    | proving optimality  | **Bucket A** — return incumbent + gap |
| incumbent + **diverging/∞** bound | unbounded suspicion | → `unbounded/`                      |
| **no** incumbent                | infeasible or hard  | **Bucket B** — elastic as classifier |

- **Bucket B** (no incumbent): run the elastic engine (I1) as a **feasibility
  classifier** (not a speed fix): min-slack = 0 → was feasible, just hard;
  min-slack > 0 finite → infeasible, hand to `infeasible/` with the edit list for
  free; elastic itself infeasible → conflict in structural rows.
- 🔬 **Open:** threshold / UX for "diverging bound" → unbounded suspicion.
- **No ETA ever** — MILP exposes no honest time-remaining (gap closes
  non-monotonically; node counts give no fraction-done). Report the gap (a fact),
  never a time estimate.

**Why the elastic engine does NOT transfer to speed:** minimizing slack has no
theory tying it to solve time; the barely-violated constraint and the
tree-exploding constraint are usually different; loosening *widens* the region →
can hand the solver more nodes. The real speed lever is tightening / reformulating
**structural** rows (loose Big-M) — the mirror of infeasible — acknowledged
future work, not scoped here.

**Deps:** S2, I1.

---

## S4 · Anytime objective→constraint switch (Bucket A)

**Goal.** Convert the hard optimality proof into a sequence of warm-started
feasibility checks: replace `maximize f` with `f ≥ b` (init `b` at the
incumbent's objective), tighten `b`, re-solve warm-started, repeat until
infeasible; return the last feasible solution (binary-search `b` within
`[incumbent, ObjBound]`). Exact and anytime — always holds the best feasible
solution so far, so it does *not* smuggle in a runtime prediction.

- 🔬 **Probe:** confirm HiGHS warm-start support (Gurobi yes).

**Deps:** S3.
