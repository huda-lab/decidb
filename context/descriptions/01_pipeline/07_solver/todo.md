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

**Still open, and small.** Native ABS is ~10% slower than its lowering on the Q10 shape
(50K rows: 2,458 ms against 2,238 ms, three runs each, tight variance). ABS is *not*
gated by derivability — its arm is still chosen by capability alone. The 10% is real and
reproducible but an order of magnitude smaller than the MIN/MAX effect, and the cause
has not been isolated. Worth a look; not worth guessing at.

Note what closing it would cost, which was not obvious when this was written. Gating ABS
by derivability means allocating its Big-M sign indicator per-link at stage 06 like the
others, which means flattening the ABS lowering from a per-link rewrite into a per-row
one — the exact per-row cost measured and *removed* above. So the trade is a known 10%
against a known representation regression, on an unisolated cause. Measure the cause
first; it may be neither.

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
