# Stage 05 — Optimizer: open work

The value rewrite and the `IN` rewrite are exact today; each item below makes the model
smaller or the language wider without changing an answer that runs now.

## 1. One helper for a reducer that several clauses hold

**Why.** The value rewrite gives each occurrence of a reducer in a clause its own helper
(`Helpers::Find` matches on the clause id and `when`). Two clauses holding the same
reducer over the same rows, `x <= 0.5 * sum(x) AND x >= 0.1 * sum(x)`, get two helpers
and two definition rows that say the same thing.

**Ruling (owner, 2026-10-27).** Share one helper. A definition carries one clause's provenance
(`source_clause_id`, `removal_group_id`), so the shared helper needs a rule for both: a DROP of one clause must not
remove the definition the other still reads, and DIAGNOSE must keep naming the helper by
its reducer.

**Test.** A model dump of the query above shows one helper column and one definition
row; DIAGNOSE and a DROP of either clause still name and keep the helper; the answer
matches the oracle on both solvers.

**done.md.** "The value rewrite — `RewriteScopedReducers`" (and the helper section of
`../06_model_formulation/done.md`).

## 2. A box per helper instance

**Why.** A helper keyed by a `by` key has one instance per group, but one column box: the
cover of every group's reach (`DeriveReducerValueBounds`, stage 06), because the column box
is per decision, not per instance. A small group's instance is boxed as wide as the
largest group's, which loosens the LP and the closing Big-M of a MIN/MAX helper, read off
that box.

**Ruling (owner, 2026-10-27).** Deferred until measured. Start it only when the helper-box measurement
(queue §11) shows a model where the shared box slows the solve. Where per-instance bounds live: `SolverInput` carries one
`lower_bounds` / `upper_bounds` entry per decision and stage 06 expands it across
`NumInstances`, so this needs a per-instance box in the stage 06/08 contract, or a box per
helper instance stated as rows.

**Test.** A dump of a helper over groups of different sizes shows each instance boxed by
its own group's reach and a smaller closing Big-M; the answer is unchanged on both
solvers.

**done.md.** `../06_model_formulation/done.md`, "Helper decisions of the value rewrite".

## 3. `IN` under `per K:` once per class

**Why.** `RewriteInDomain` keeps the clause's `when` on the rows it emits but not its
`per K:`, so `per g: x IN (1, 2)` over a keyed `x` emits one indicator set per kept row
rather than per class: three rows in two classes give six indicators where four would
do. It is exact, because the key determines the decision and the domain.

**Decision needed.** None in the language. The indicators are row-scoped auxiliaries;
emitting them per class needs them keyed by `K` (a scope like a helper's) and the `per K:`
wrapper kept on the generated rows, as the MIN/MAX equality's easy half keeps it.

**Test.** The dump of `per g: x IN (1, 2)` over three rows in two classes shows four
indicators and four rows (a pick-one row and a linking row per class), not six and six;
DIAGNOSE still names the clause.

**done.md.** "NORM and IN — `RewriteNorm` / `RewriteInDomain`".
