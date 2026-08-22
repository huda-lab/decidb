# Stage 07 — Solver: open work

---

## The scaling win is not yet measured

**Status**: the mechanism shipped; the benchmark that motivated it has not been run.

The general-constraint and indicator channel now exists — ABS, MIN/MAX and `<>` all
reach Gurobi natively when it declares them, with the Big-M path intact as the fallback
for every other backend. What has *not* been measured is the thing that justified
building it: the hard-direction MIN/MAX encoding is the one place where the LP
relaxation demonstrably weakens with row count, and that is what caps Q9's benchmark
scale. The encoding analysis is in
[`../../06_issues/code_quality/todo.md`](../../06_issues/code_quality/todo.md).

**Test**: Q9 at 5K / 7.5K / 15K / 30K against the recorded Big-M curve, on both
backends. HiGHS must be unchanged — it declares no construct, so its model is
byte-identical. Gurobi must flatten, because `z = MAX(t..)` has no Big-M whose
relaxation can loosen.

This is a long CPU-bound run; it is worth doing deliberately rather than as part of a
routine suite.

**Correctness is already covered.** `test_native_constructs.py` A/B-tests every gated
shape against its lowering path on one machine (`DECIDB_NATIVE_CONSTRUCTS=off`), and the
golden corpus shows HiGHS byte-identical throughout. What is missing is only the speed
claim.

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
