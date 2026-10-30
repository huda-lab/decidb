# Stage 04 — Canonicalizer: open work

Each item below changes which shape `ClassifyScoped` or `PeelScale` gives a comparison,
which only this stage decides (`done.md` §3.7, §3.10).

---

## 1. `AVG` under `<>` beside another term, or with its own `when` or `per`

**Today.** `avg(x) + sum(y) <> 3`, `x <> avg(x)`, `avg(when c: x) <> 2`,
`avg(per D: x) <> 2` and an `AVG` beside a product of decisions are refused by
`ClassifyScoped`: *"DECIDE: AVG in a
<> constraint is not supported yet beside another term or with its own when or per"*.
`<>` excludes an `AVG`'s value by scaling the bound with the group's row count
(`SUM(e) <> K * n`), which is exact only when every term is a plain `AVG` reading every
row of the group.

**Design.** Two routes. The VALUE shape would hold each `AVG` in a helper decision, but
an average's helper is fractional, and the value rewrite refuses `<>` over a fractional
helper (`DecideValueIsWholeNumbered`). Scaling every term by its own reducer's row count
needs a common multiple of the counts, which is data, and enlarges the Big-M of the `<>`
disjunction.

**Decision needed.** Lift it, and by which route, or leave it refused.

**Test.** The shapes above, oracle-judged on both solvers
(`test_scope_value_rewrite.py` or `test_scope_bare_clauses.py`), and the refusal kept
for any shape not lifted.

**Update.** `done.md` §3.10.

---

## 2. A decision as a factor on a reducer

**Today.** `b * sum(x) <= 4`, `b` a query-wide decision, is refused by `PeelScale`:
*"DECIDE constraint: 'b' is a decision, so it cannot multiply SUM(x). Only constants and
query-wide values can scale SUM/AVG/MIN/MAX."*, in both clauses. A product of a decision
and a reducer is not a scale, and no shape holds it.

**Design.** The VALUE shape would hold `sum(x)` in a helper decision `v` and leave
`b * v`, an ordinary product of two decisions that stage 05's bilinear rewrite takes when
one of them is `BOOL` (and that the quadratic path refuses otherwise). `PeelScale` would
stop refusing a decision factor in a constraint and let `ClassifyScoped` send the
comparison to VALUE.

**Decision needed.** Whether this enters the language (syntax_reference.md §5 states the
refusal).

**Test.** `test_canonicalize_scale.py::test_decision_factor_rejected` and
`test_decision_factor_in_objective_rejected` turn into answer tests for the shapes
lifted, oracle-judged on both solvers.

**Update.** `done.md` §3.7 and §3.10, `../../00_project_overview/syntax_reference.md` §5.

---

## 3. A keyed decision beside a reducer without a helper

**Today.** The facility-location link `per D: sum(ship) by (D) <= cap * open`, with
`open` declared `per D:`, takes the VALUE shape: a keyed decision beside a reducer is
not one the aggregate shape holds. The value rewrite adds one helper decision per `by`
group and its definition row, so EXPLAIN shows `sum(ship) by (D) becomes a helper
decision per group`.

**Design.** When the `by` key fixes the keyed decision — one instance of `open` per
group, which holds at plan time when the `by` key holds the decision's key columns — the
comparison is one row per group with `open`'s column for that group: the aggregate shape
with a decision beside the reducers. `ClassifyScoped` would admit such a decision into
AGGREGATE, and the linear form and execution would place the group's instance column in
the group's row. A group whose rows span two instances of `open` is two variants (D64),
so a run-time variant of the shape would emit one row per (group, instance).

**Decision needed.** Whether to add the shape, and whether to admit it only by the
plan-time key comparison or also, through the signature, at run time.

**Test.** The facility-location query against its VALUE-shape answer (same optimum,
fewer columns and rows in the dump), on both solvers; a golden entry for the smaller
model.

**Update.** `done.md` §3.10, `../05_optimizer/done.md` (the value rewrite no longer sees
the clause), `../06_model_formulation/done.md`.

## 4. A frame that may select no position inside a reducer over decisions

**Why.** `sum(at(previous: x) over (t.period)) <= 4` drops the first period's member,
decisions and all, and a group left with no member makes the reducer absent, which skips
its instance (`syntax_reference.md` §5.6, plan §3.5). Phase 4 states neither, so
`PlaceFrames` refuses it: *"DECIDE: at(previous: x) over (t.period) may select no
position inside sum(at(previous: x) over (t.period)), a reducer over decisions, which is
not supported yet; give it an else, or make its timeline cyclic"* — in the objective too.
Deck example 3 (`per T.productID: sum(at(previous: inventory) over (...)) by
(T.productID) <= P.priorInventoryLimit`) is this shape. A range that may read nothing is
refused the same way (`sum(sum(from 2 next to 3 next: x) over (t.period))`), its remedy
worded by what makes it read nothing (`DecideFrameAbsenceRemedy`).

**What lifting it needs.** The reducer's members masked by the frame's presence (its
`PRESENT` column joined to the terms' filter, as a bound-side reducer's `when` already is),
AVG dividing by the members left, a MIN/MAX's members dropped, and an absent reducer
skipping its instance or dropping the outer member; for a range, whose helper has no
definition at a window that reads nothing, the same presence on the members reading it —
phase 6 of `09_anr_language/anr_algorithm_and_frames_plan.md`.

**Test.** Deck example 3 and slide 50's `per (): sum(at(previous: inventory) over (...))
>= 10`, judged by `_anr_oracle`; `test_anr_frames.py::TestPointErrors::
test_phase_6_shapes_are_refused` and `TestRangeErrors::
test_a_range_that_may_read_nothing_inside_a_reducer_over_decisions` become answer tests.
