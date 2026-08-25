# Stage 07 — Solver: open work

---

## The scaling win was measured — and it is the other way round

**Status**: measured 2026-08-23, and the mechanism was re-gated as a result. This is the
record of what the numbers said. Nothing here is open.

The general-constraint channel shipped without the benchmark that motivated it. The
claim was that the hard-direction MIN/MAX encoding is where the LP relaxation weakens
with row count, so stating `z = MAX(t..)` natively would flatten Q9's curve. Q9 was run
at 5K / 7.5K / 15K / 30K on Gurobi, native against the lowering, `medium.db`, solver
time only:

| rows | native | lowering | objective |
|---|---|---|---|
| 5,000 | 183 ms | 93 ms | identical |
| 7,500 | 302 ms | 93 ms | identical |
| 15,000 | 334 ms | 205 ms | identical |
| 30,000 | 787 ms | 456 ms | identical |

**There was no curve left to flatten.** The 2026-08-18 auxiliary-boxing fix had already
done that, and it is what the premise was written before. Both arms are near-linear, and
at Gurobi's default settings both finish in **0–1 nodes** — the relaxations are exact, so
there is no search to improve. Q9 cannot test the claim it was chosen to test.

**Native is the slower arm, structurally.** Two reasons, both measurable:

- A general constraint relates *columns*, so every member expression is pinned to a
  fresh one. Presolve cannot substitute a column a general constraint reads, so the
  copies survive: at 15K rows the native model presolved to 60,001 columns / 45,003 rows
  against the lowering's 30,001 / 15,003.
- `z = MAX(t..)` is an *equality*, so the backend expands both directions; the lowering
  emits only the direction the clause needs. Hand-lowering the same dump one-sided and
  two-sided at 30K rows: 0.85s against 1.36s.

The **constraint** side is far worse than the objective side. `MAX(keep * price) >= K`,
native against the lowering: 912 ms vs 90 ms at 15K, and 3,357 ms vs 93 ms at 30K —
native's cost grows with row count while the lowering stays flat.

**Where the claim was right.** With `Presolve=0 Cuts=0 Heuristics=0` the ranking
inverts: native takes 125–341 nodes against the lowering's 490–2,585, and runs 5–15x
faster. The Big-M formulation really is the harder one to search. Gurobi's default
presolve and cuts close that gap completely on this shape, which leaves model size as
the only thing that still separates the arms — and there the lowering is smaller.

**What changed as a result.** Native MIN/MAX is now the *fallback*, taken per clause
only where the lowering has no valid Big-M. The capability payoff is untouched:
`MAX(x) >= 5` over an unbounded `x` still answers, because that clause has no lowering.
Bounded queries take the smaller model. See
[`../05_optimizer/done.md`](../05_optimizer/done.md) §0 for the policy and
[`../08_execution/done.md`](../08_execution/done.md) for where it is applied.

**ABS was the one arm left ungated, and it stays that way.** That question is now
answered in its own section below; nothing about ABS is open here.


---

## Derivability-gated ABS — measured and declined

**Status**: measured 2026-08-24 on Gurobi 12.0.3, not built, and this is the record of
why. Do not re-open without a new Gurobi version.

MIN/MAX takes its native arm only where the lowering has no valid Big-M (above). ABS
never got that gating: its arm is chosen by backend capability alone, and native ABS
measured slower than its own lowering. The question was whether to gate it the same way.

**The gap is real, and it grows with the data.** Q10's shape (`MAXIMIZE SUM(ABS(adj -
l_quantity))` with a hard `SUM(ABS(adj - 15)) >= K` constraint) on `medium.db`, three
runs per point, `DECIDB_NATIVE_CONSTRUCTS=force` against `=off`, solver time only:

| rows | native | lowering | gap | ratio |
|---|---|---|---|---|
| 1,000 | 63 ms | 63 ms | 0 ms | 1.00x |
| 5,000 | 304 ms | 221 ms | 83 ms | 1.38x |
| 15,000 | 736 ms | 650 ms | 86 ms | 1.13x |
| 50,000 | 2,413 ms | 2,196 ms | 217 ms | 1.10x |
| 500,000 | 32,267 ms | 30,189 ms | 2,078 ms | 1.07x |

Identical objective at every size. Read the **gap**, not the ratio: the ratio falls only
because the query's own cost grows alongside the penalty. Past 50K the gap is linear in
row count — 10x the rows, 9.6x the gap. Below 50K it is flat at roughly 85 ms.

**The cause is Gurobi's presolve, not our model.** Rebuilt in gurobipy on the shape
`DECIDB_DUMP_MODEL` shows us emitting, at 50K rows:

| | native | lowering |
|---|---|---|
| what DeciDB hands over | 250,000 cols / 300,002 rows / **0 binaries** / 700,000 nz | 250,000 cols / 400,002 rows / 100,000 binaries / 1,100,000 nz |
| after Gurobi presolve | **345,565 cols** / 295,567 rows / 79,113 bin / 849,356 nz | **208,226 cols** / 316,454 rows / 79,113 bin / 947,356 nz |
| nodes | 1 | 1 |

Both arms finish at the root, so search quality is not involved — the relaxations are
exact either way. Gurobi expands every `aux = |t|` into its own binary formulation during
presolve: the 79,113 binaries appear from nothing, and the presolved native model ends up
137,000 columns *larger* than the presolved lowering. Our lowering presolves **down**
(250,000 → 208,226 cols); Gurobi's expansion of the same idea presolves **up**. Timed
separately, presolve is 2,244 ms native against 1,374 ms — an 870 ms delta that accounts
for essentially the whole 862 ms difference in solve time. Model construction is ours and
holds a constant 5% share of the gap (145 ms vs 41 ms at 500K).

**Declined, because we hand Gurobi the better model and it makes a worse one.** The
native arm is smaller on every axis we control and it is the *truthful* one: it states
the absolute value the user wrote. Routing around a third-party presolve inefficiency
would put a vendor workaround permanently into our formulation layer, to be maintained,
A/B tested on both arms and carried in the golden baselines — and silently inverted the
moment Gurobi improves `GenConstrAbs` presolve.

The MIN/MAX precedent above does not transfer. That re-gating was driven by 1.7–3.3x on
the objective side and **41x** on the constraint side, with native's cost growing while
the lowering stayed flat. 7% on a shape where both arms solve at the root is a different
kind of number, and the same construct family is not reason enough to make it the same
call.

**What it would have cost**, since both routes were priced:

- Allocating the sign binary unconditionally at stage 05 and routing per link at stage 08
  (`link.range_unbounded` is already computed there, before the branch). About a dozen
  lines, no flattening — but it re-introduces for ABS the stage-05 guess that B5/S4 just
  removed from `<>` and MIN/MAX.
- Moving the binary to the global block, matching `<>` and MIN/MAX. Structurally
  consistent, but a global column is per-row, so the lowering's four compressed per-link
  rows flatten into 4xR rows — the per-row cost measured and removed above.

**Re-check trigger**: a new Gurobi major version, or any release noting general-constraint
presolve changes. Nothing in DeciDB needs to change for the finding to flip.

**Done file**: none — nothing shipped.

---

## SOS1 for `IN` — measured and declined

**Status**: investigated, not built, and this is the record of why. Do not re-open
without new evidence.

`x IN (v1..vk)` lowers to `k` binaries with `SUM(z_i) = 1` and `x = SUM(v_i * z_i)`.
The obvious "native" move is to declare the `z_i` as an SOS1 set so Gurobi can branch on
the set rather than on one binary at a time.

**It does not help, because there is nothing to branch on.** The formulation's LP
relaxation is already integral, so the solve finishes at the root — and branching is
SOS1's only lever.

Measured with gurobipy on the exact model shape DeciDB emits (k binaries, the
cardinality row, the linking equation, plus knapsacks over the rows), at
`k = 20 / 40 / 80` and 60–400 rows, with and without an SOS1 declaration:

| Configuration | Nodes | Objective | SOS1 speedup |
|---|---|---|---|
| default Gurobi | 0 | identical | 0.53× – 0.85× |
| `Presolve=0`, `Cuts=0`, `Heuristics=0` | 1 | identical | 1.01× – 1.44× |

Nine configurations, never a meaningful win, and mostly a loss — the declaration is
redundant work on a model Gurobi already recognizes as a clique.

**What it would have cost**: a third native model concept (SOS sets) beside general and
indicator constraints, a loader symbol, an adapter branch, and link metadata from stage
05, which currently lowers `IN` completely and records nothing to gate on.

There is therefore no `in_list` field in `SolverConstructSupport`, and the comment where
one used to sit points back here. A permanently-false flag nobody reads cannot be told
apart from one whose implementation is merely still pending, so the measurement is the
record instead.

That is the capability rule applied as written — a flag has to be A/B-verifiable, and
this one is verifiable and fails. If `IN` is ever worth accelerating, the lead is the
*formulation*, not the branching hint.

**Done file**: none — nothing shipped.
