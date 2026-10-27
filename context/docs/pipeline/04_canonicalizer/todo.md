# Stage 04 — Canonicalizer: open work

Each item below changes which shape `ClassifyScoped` or `PeelScale` gives a comparison,
which only this stage decides (`done.md` §3.7, §3.10).

---

## 1. A decision as a factor on a reducer

**Today.** `b * sum(x) <= 4`, `b` a query-wide decision, is refused by `PeelScale`:
*"DECIDE constraint: 'b' is a decision, so it cannot multiply SUM(x). Only constants and
query-wide values can scale SUM/AVG/MIN/MAX."*, in both clauses. A product of a decision
and a reducer is not a scale, and no shape holds it.

**Design.** The VALUE shape would hold `sum(x)` in a helper decision `v` and leave
`b * v`, an ordinary product of two decisions that stage 05's bilinear rewrite takes when
one of them is `BOOL` (and that the quadratic path refuses otherwise). `PeelScale` would
stop refusing a decision factor in a constraint and let `ClassifyScoped` send the
comparison to VALUE.

**Ruling (owner, 2026-10-27).** Lift it for a `BOOL` factor only (one McCormick helper over the reducer's
value). An `INT` or `REAL` factor stays refused (../../language/guide.md §5 states the
refusal).

**Test.** `test_canonicalize_scale.py::test_decision_factor_rejected` and
`test_decision_factor_in_objective_rejected` turn into answer tests for the shapes
lifted, oracle-judged on both solvers.

**Update.** `done.md` §3.7 and §3.10, `../../language/guide.md` §5.

---

## 2. A keyed decision beside a reducer without a helper

**Today.** The facility-location link `per D: sum(ship) by (D) <= cap * open`, with
`open` declared `per D:`, takes the VALUE shape: a keyed decision beside a reducer is
not one the aggregate shape holds. The value rewrite adds one helper decision per `by`
group and its definition row, so EXPLAIN shows `sum(ship) by (D) becomes a helper
decision per group`.

**Design.** When the `by` key determines the keyed decision — one instance of `open` per
group, which a DETERMINES scope check can state — the comparison is one row per group
with `open`'s column for that group: the aggregate shape with a decision beside the
reducers. `ClassifyScoped` would admit such a decision into AGGREGATE, and the linear
form and execution would place the group's instance column in the group's row.

**Ruling (owner, 2026-10-27).** Add the shape. The determination is a plan-time key comparison: the `by`
key is the decision's `per` key.

**Test.** The facility-location query against its VALUE-shape answer (same optimum,
fewer columns and rows in the dump), on both solvers; a golden entry for the smaller
model.

**Update.** `done.md` §3.10, `../05_optimizer/done.md` (the value rewrite no longer sees
the clause), `../06_model_formulation/done.md`.
