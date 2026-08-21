# Stage 05 — Optimizer: open work

---

## Infeasible diagnostics: atomically drop NORM-L0 and IN formulations

**Pointers**: `src/decidb/utility/decide_diagnostic_engines.cpp` and
`src/include/duckdb/decidb/ilp_model.hpp`.

An `IN (...)` restriction and an L0 `norm(...)` expand to a group of cardinality,
indicator, and linking rows. Infeasibility repair must never loosen or
independently drop one of those rows: that would describe neither the original
SQL clause nor a sound relaxation of it.

**Required behavior**: record each formulation as one source-level,
**drop-only** repair group. A diagnosis may propose one `DROP <original SQL
clause>` edit for the entire group; it must not propose `LOOSEN`, a partial drop,
or an internal indicator equation.

**Design constraint**: the existing `<>` removal path is specialized and cannot
be reused as-is. Add a general grouped-removal contract with a verified safe
neutralization strategy before exposing this repair; do not use an arbitrary
Big-M.

**Test**: infeasible IN and L0 queries report one source-SQL DROP action, and
prove all generated rows disappear together on both solver backends.

---

## No cost-based backend or formulation selection

**Pointers**: `SelectSolverBackend()` in `src/decidb/utility/ilp_solver.cpp`;
`is_easy` computation in `src/optimizer/decide/decide_optimizer.cpp`.

Two selections are made statically today:

1. **Backend** — Gurobi whenever available, regardless of problem
   characteristics. That is defensible (Gurobi is faster on every measured
   workload) but it is not a decision, it is a default.
2. **MIN/MAX formulation** — easy vs hard follows purely from direction and the
   factor's sign. It does not consider row count, even though the hard encoding's
   relaxation is known to weaken as rows grow (see
   [`../07_solver/todo.md`](../07_solver/todo.md)).

**Decision needed**: whether formulation choice should consult
`estimated_cardinality`. It is available on the operator, and the hard-MIN/MAX
weakness is a function of row count specifically — but a cost model that is wrong
is worse than a static choice that is predictable, and DeciDB has no calibration
data for one.

**Test**: whatever rule is chosen must not change any of the 80 golden models
below the threshold it introduces.

**Done file**: `done.md` §2 (MIN/MAX) and `../07_solver/done.md` §2 (backend).

---

## Native MIN/MAX: what the spike found

**Status**: SUPERSEDED — native MIN/MAX shipped, and the answer below is why it took
the shape it did. Kept because the negative result is the useful part.

**What actually happened.** The spike asked whether stage 05 could be told *not* to
rewrite. Finding (1) said no: the prepared linear form has no vocabulary for a reducer
kind, so a surviving `MAX` aggregate is read as a sum by everything downstream. The
implementation therefore took the other route — stage 05 keeps rewriting and tagging
exactly as before, and the *gate* lives at stage 08, choosing between the Big-M rows
and a general constraint from the same tag. Findings (4) and (5) evaporate under that
design: the tag is still set, so `DecidePropagateImpliedBounds` still skips the row,
and `was_minmax_easy` still means what it meant. Finding (2) is what made the design
work at all.

The one thing the constraint side does need is to **lift the tagged row out of the
model** before the expansion, because until an arm rewrites it the row reads as
`SUM(inner)` while the clause means `MAX(inner)`.

**The question**: if the backend can express MIN/MAX natively, stage 05 should not
rewrite it. Does anything *downstream* of stage 05 require the rewrite to have
happened — box sizing, the model builder, provenance — or only the emitters?

**Method**: suppress the rewrite for `MAX(e) <= C` (and then `MAX(e) >= C`) behind a
hardcoded flag in `RewriteMinMaxInConstraint`, let the aggregate reach execution
untouched, and follow what breaks.

**Answer: only the emitters.** But four things have to move with it.

1. **The first blocker is stage 05 itself, not stage 08.**
   `DecideLinearFormBuilder::ExtractAggregateConstraintTerms`
   (`decide_linear_form.cpp:1002`) throws an `InternalException` on any aggregate
   that is not SUM. The prepared linear form has **no vocabulary for a reducer
   kind**: `DecideConstraint` carries `lhs_is_aggregate` (a bool) and
   `minmax_indicator_idx`, and every consumer reads an aggregate LHS as a sum. So
   "leave the tree alone" is not a viable record of a native construct. The reducer
   kind has to become a field on `DecideConstraint`, the way `abs_y_idx` already
   records a tagged ABS.

2. **Nothing downstream demands the indicator variables.** Past the flattener the
   constraint flows through `AnalyzeConstraint` → `EvaluatedConstraint` →
   `SolverModel::Build` as an ordinary aggregate row: no assert, no crash.
   `LinearizeMinMaxIndicators` is already guarded by `!minmax_indicator_links.empty()`
   (`physical_decide.cpp:2698`), so with no indicators it simply does not run. This is
   what makes the gate design viable at all.

3. **`AbsorbVariableBounds` is not a constraint on the design.** It sizes the box
   over `decide_variables` after every other pass, and suppressing a rewrite creates
   *fewer* auxiliaries, never a later one. Its "must run last" rule is untouched.

4. **`DecidePropagateImpliedBounds` is the one real hazard.**
   `ilp_linearization.cpp:189` skips any constraint carrying `minmax_indicator_idx` /
   `ne_indicator_idx` / `abs_y_idx`, because such a row is not the plain sum it looks
   like. A natively-emitted MIN/MAX row carries none of those, so it would be read as
   an ordinary aggregate and could produce a wrong implied bound. That skip must
   extend to the reducer kind from (1). This is the finding most likely to be missed,
   because it fails **silently** rather than loudly.

5. **The empty-WHEN rejection rides on the rewrite.** `was_minmax_easy`
   (`decide_linear_form.cpp:1272`, consumed at `physical_decide.cpp:1928`, `:1945`,
   `:2126`) exists only because the easy rewrite strips the aggregate. A native
   MIN/MAX keeps its aggregate, so `lhs_is_aggregate` covers those three sites — but
   the flag must stay for the lowered path, which still needs it.

6. **Diagnosis over a native row has nothing to slacken.** The row is stamped with
   ordinary aggregate provenance, and a construct that stays native has no matrix row
   for the elastic engine to relax. Confirms the already-parked item rather than
   adding a new one.

**Done file**: `done.md` §2 (MIN/MAX), once the work lands.
