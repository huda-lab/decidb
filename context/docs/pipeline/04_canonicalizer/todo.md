# Stage 04 — Canonicalizer: open work

Each item below changes which shape `ClassifyScoped` or `PeelScale` gives a comparison,
which only this stage decides (`done.md` §3.7, §3.10).

---

## CAN-2 — A keyed decision beside a reducer without a helper

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

**Decision needed (architect review done).** The shape is the aggregate shape with a tuple term of one value per
group; it does not wait for `../../issues/code_quality.md` § CQ-4. Four choices:
1. The classifier needs the clause's `per` wrapper and the key lists, but the shape is recomputed from the bare
   comparison at five sites (`decide_linear_form.cpp`, `direct_ir.cpp`, `decide_rewrite_rhs_window.cpp`, twice in the
   canonicalizer). Decide once in `CanonicalizeTreeInternal` with the enclosing scope and stamp a tag (as
   `VALUE_REWRITE_TAG` is), or pass the wrapper to every site? Recommended: the tag.
2. What makes the coefficient (`cap` in `cap * open`) one value per group. A wrapper's DETERMINES check gives one per
   class, not per group (`per S.shipmentID: sum(ship) by (D) <= S.cap * open` passes it while `cap` varies in a
   depot). Admit when the wrapper key, the `by` key and the decision's `per` key are equal; also admit a query-wide
   coefficient with no wrapper? Without a wrapper a data coefficient stays VALUE (one constraint per row).
   Recommended: both cases.
3. One `CountsOnce(term)` test replaces the five copies in the model builder and `linearization_not_equal.cpp`: keep
   the "not row-scoped" guard, or throw on reduction arrays shorter than the terms? Recommended: keep the guard.
4. Extract `CountsOnce` first as its own change, or inside this task? Recommended: first.

**Test.** The facility-location query against its VALUE-shape answer (same optimum,
fewer columns and rows in the dump), on both solvers; a golden entry for the smaller
model.

**Update.** `done.md` §3.10, `../05_optimizer/done.md` (the value rewrite no longer sees
the clause), `../06_model_formulation/done.md`.
