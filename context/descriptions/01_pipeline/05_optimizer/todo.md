# Stage 05 — Optimizer: open work

The value rewrite and the `IN` rewrite are exact today; each item below makes the model
smaller or the language wider without changing an answer that runs now.

## 1. One helper for a reducer that several clauses hold

**Why.** The value rewrite gives each occurrence of a reducer in a clause its own helper
(`Helpers::Find` matches on the clause id and `when`). Two clauses holding the same
reducer over the same rows, `x <= 0.5 * sum(x) AND x >= 0.1 * sum(x)`, get two helpers
and two definition rows that say the same thing.

**Decision needed.** A definition carries one clause's provenance (`source_clause_id`,
`removal_group_id`). A shared helper needs a rule for both: a DROP of one clause must not
remove the definition the other still reads, and DIAGNOSE must keep naming the helper by
its reducer.

**Test.** A model dump of the query above shows one helper column and one definition
row; DIAGNOSE and a DROP of either clause still name and keep the helper; the answer
matches the oracle on both solvers.

**done.md.** "The value rewrite — `RewriteScopedReducers`" (and the helper section of
`06_model_formulation/done.md`).

## 2. A box per helper instance

**Why.** A helper keyed by a `by` key has one instance per group, but one column box: the
cover of every group's reach (`DeriveReducerValueBounds`, stage 06), because the column box
is per decision, not per instance. A small group's instance is boxed as wide as the
largest group's, which loosens the LP and the closing Big-M of a MIN/MAX helper, read off
that box.

A nest widens it with depth: an outer helper's members are the inner one's instances, each
read off the one box (`DeriveReducerValueBounds`), and the bound a helper passes down to
the helpers inside it (`DeriveReducerMemberBounds`) is the loosest over its instances,
taken only when every instance it defines has one. F1's spare inner column, which no
row reads, carries the same box (`test_anr_nested.py`'s model pin).

**Decision needed.** Where per-instance bounds live: `SolverInput` carries one
`lower_bounds` / `upper_bounds` entry per decision and stage 06 expands it across
`NumInstances`, so this needs a per-instance box in the stage 06/08 contract, or a box per
helper instance stated as rows.

**Test.** A dump of a helper over groups of different sizes shows each instance boxed by
its own group's reach and a smaller closing Big-M; the answer is unchanged on both
solvers.

**done.md.** `06_model_formulation/done.md`, "Helper decisions of the value rewrite".

## 3. `IN` under `per K:` once per class

**Why.** `RewriteInDomain` keeps the clause's `when` on the rows it emits but not its
`per K:`, so `per g: x IN (1, 2)` over a keyed `x` emits one indicator set per kept row
rather than per class: three rows in two classes give six indicators where four would
do. It is exact, because the key fixes the decision and the domain.

**Decision needed.** None in the language. The indicators are row-scoped auxiliaries;
emitting them per class needs them keyed by `K` (a scope like a helper's) and the `per K:`
wrapper kept on the generated rows, as the MIN/MAX equality's easy half keeps it.

**Test.** The dump of `per g: x IN (1, 2)` over three rows in two classes shows four
indicators and four rows (a pick-one row and a linking row per class), not six and six;
DIAGNOSE still names the clause.

**done.md.** "NORM and IN — `RewriteNorm` / `RewriteInDomain`".

## 4. Refusals a later change may lift

**Why.** Six shapes reach this stage and are refused here, each with a message that
says "not supported yet" (syntax_reference.md §10.2). None is a wrong answer; each lacks
a formulation.

| Refused | Where | What lifting it needs |
|---|---|---|
| A square or a product of decisions in a member a frame may drop (`sum(power(x, 2) + at(previous: t.v) over (t.period)) <= 9`): *"DECIDE: 'sum(...)' squares or multiplies decisions in a member at(previous: t.v) over (t.period) may drop, which is not supported yet; give it an else, or make its timeline cyclic"* | `RewriteScopedReducers` | the quadratic helper definition below, or the aggregate shape dropping the member in place (phase 7 of `09_anr_language/anr_algorithm_and_frames_plan.md` folds such helpers back in) |
| A square or a product of two non-Boolean decisions inside a reducer the value rewrite replaces (`x <= 0.5 * sum(x * x)`): *"DECIDE: 'sum(x * x)' is not supported yet in this constraint: a product or square of decisions inside a reducer is accepted only where ..."* | `RewriteScopedReducers` (`decide_rewrite_scoped.cpp`) | a quadratic definition row for the helper (`v = Σ x²`), which stage 06 does not state; only the side the clause pushes `v` toward would be convex |
| A square or a product of decisions inside nested reducers (`sum(max(x * x) by (g)) <= 5`, `MAXIMIZE sum(per g: max(power(x - t, 2)) by (g))`), but the objective's direct `sum(per k: sum(e) by (k))`: *"DECIDE: 'max(x * x)' squares or multiplies decisions inside nested reducers, which is not supported yet"* (D74) | `RewriteScopedReducers` | a quadratic member of an extremum link (convex on the side the readers push the helper toward: `value >= q` for a MAX pushed down), and for SUM a quadratic equality, or the SUM inlined into a reader that sums it (phase 7 of `09_anr_language/anr_algorithm_and_frames_plan.md`) |
| A square or a product of decisions inside a flat MIN or MAX that is not a bound every member meets (`max(power(x, 2)) >= 49`, `MAXIMIZE max(power(x, 2))`, `sum(x) + max(power(x, 2)) <= 20`), or in a term beside one: *"DECIDE: 'max(power(x, 2))' squares or multiplies decisions inside MIN or MAX, which is not supported yet; ..."* | `RefuseNonlinearReducer` (`decide_rewrite_minmax.cpp`) | an extremum link over quadratic members (an epigraph `z >= q` on the convex side, indicators with quadratic rows on the other, Gurobi only), and the composed path stating a SUM term's square in its row or the objective's Q matrix |
| A decision beside a `MIN` or `MAX` in the objective (`MAXIMIZE max(x) + s`): *"DECIDE: 's' beside MIN or MAX is not supported yet"* | `RewriteComposedMinMaxObjectiveTop`, through `WalkComposedLhs` (`decide_rewrite_minmax.cpp`) | the composed objective's extremum column beside a once-counted term, as the constraint's VALUE shape already does with a helper |
| A square, or a product of two decisions outside a reducer in the objective, other than two distinct query-wide decisions or a product with a `BOOL` (`MAXIMIZE sum(x) + x * y`, `+ s * s`): *"DECIDE objective: 'x * y' multiplies decisions outside a reducer, which is not supported yet"* | `ExtractAggregateObjectiveTerms` (`decide_linear_form.cpp`, the linear-form flattening) | a once-only reading of the product in the model: a per-row product needs the objective's signature (D66) to give it one instance over the kept rows, then a quadratic or McCormick term counted once |

**Decision needed.** Which of these enter the language, and in what order.

**Test.** For each lifted shape, an oracle- or brute-force-judged test on both solvers
(`test_scope_value_rewrite.py`, `test_scope_objective.py`); the pinned refusals
(`test_scope_value_rewrite.py::test_a_product_of_decisions_inside_a_rewritten_reducer_is_refused`,
`test_scope_reducer_by.py`'s decision-beside-max case, `test_anr_nested.py::test_refused_nests`,
`test_min_max.py::test_a_min_or_max_over_a_square_is_refused`,
`test_per_objective.py::test_an_extremum_of_a_square_inside_a_nested_objective_is_refused`,
`test_scope_objective.py::test_objective_tuple_term_errors`) become answer tests.

**done.md.** "The value rewrite — `RewriteScopedReducers`", "Composed MIN/MAX —
`RewriteComposedMinMax`", §1a; `../../00_project_overview/syntax_reference.md` §10.2.
