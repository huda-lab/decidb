# Stage 05 — Optimizer: open work

Items 1–3 make the model smaller or the language wider without changing an answer that runs now. Item 4 fixes wrong
answers.

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

## 4. Lift a quadratic MIN/MAX body into a row helper

**Why.** Every extremum site reads its members as linear, and nothing establishes that: the hard rows
(`EmitHardMinMaxClause`, then `BuildMinMaxGroups` in `linearization_minmax.cpp`, which reads only
`variable_indices` / `row_coefficients`), the composed terms (`ComposedMinMaxTerm::inner_terms`), the flat objective
(the path at `linearization_minmax.cpp` that runs only when the objective has a linear term) and the two-level
objective. A degree-2 body after `RewriteBilinear` (`power(x, 2)`, an INT×INT product) loses its square there.
Symptoms: [bugs § MIN/MAX bodies](../../issues/bugs.md#wrong-answers-minmax-bodies-and-solver-tolerance).
`RewriteAbs` already settles the same problem for ABS: it lifts each ABS into a row helper before the MIN/MAX
rewrites run.

**Ruling (owner, 2026-10-28).** Fix it, not refuse it (reverses 2026-10-27). Design:

- **Lift** (auxiliary-variable reformulation, as `decide_rewrite_abs.cpp` does with `AddAuxiliaryDecision` /
  `ReplaceWithDecision` / `ScopeHelperRow`): one helper `t` per member row and one quadratic row relating it to the
  body. The extremum sees the bare column `t`.
- **Direction follows the push** on the member, which equals the push on its reducer (MIN, MAX, SUM and AVG never
  decrease when a member grows): push down → `t >= q`; push up → `t <= q`; no push (`=`, a factor of unknown sign)
  → `t = q`. The convex side stays convex.
- **Called where the push is known**, five sites: `EmitHardMinMaxClause` (hard MAX pushes up, hard MIN down; covers
  the hard half of `=` and negative factors), `WalkComposedLhs` (covers composed MIN/MAX terms, a composed SUM term
  with a squared body, and the composed objective), the flat objective and the two-level objective's inner reducer
  (push from the sense), and the value rewrite's MIN/MAX links (from `need_envelope` / `need_closing`) only if a
  query reaches them. The easy plain constraint (`max(q) <= K`) is not lifted: it keeps a quadratic row per member.
- **Helper type** INTEGER when the body is integral, as for ABS, so a strict `>` over an INT body stays sound.
- **Gurobi's native MAX/MIN, no box derivation.** A helper gets no derived box. HiGHS takes no quadratic rows, so
  Gurobi is the only backend that sees a lifted clause, and its native general MAX/MIN (`use_native_constructs`)
  needs no box; the "sole column" path in `ExtremumArgumentColumn` passes `t` itself. With
  `DECIDB_NATIVE_CONSTRUCTS=off` the Big-M arm finds `t` unboxed and refuses, naming the user's decision, not `t`.
  Gurobi has no native form that takes the body directly (checked against Gurobi 13: `GRBaddgenconstrMax` takes
  columns, `GRBaddgenconstrNL` has no MAX/MIN opcode, indicator rows are linear), so the helper stays.
- **No convexity flag.** Gurobi 13 solves non-convex constraint rows at its default `NonConvex`. A backend that takes
  only convex rows is a roadmap item.
- **HiGHS** refuses through the solver gate, which already predicts the lifted rows as quadratic constraints;
  `DescribeGap` gets a branch so the message names the MIN/MAX, not "a SUCH THAT clause squares…".
- **EXPLAIN** names the lift with its own `RewriteAccount` kind, as ABS does, and stops claiming indicator rows that
  a quadratic body never had.

First verify that the native arm covers all five sites, the two objective forms included. A site it does not cover
needs a box for `t` (interval arithmetic over the body); stop and ask before adding one.

**Test.** Oracle-compared on Gurobi, one per decision point; HiGHS cases are error tests:

| Decision point | Query |
|---|---|
| push up, `t <= q` | `max(power(x,2)) >= 49`, INT, `x <= 7` (today: infeasible) |
| push down, `t >= q` | `sum(x) + max(power(x,2)) <= 20`, MAXIMIZE `sum(x)` (today: internal error) |
| no push, `t = q` | `sum(x) + max(power(x,2)) = 30`; `(SELECT -1) * max(power(x,2)) <= -25` |
| INT helper | `max(power(x,2)) > 48`, INT, gives a 7 |
| hard MIN site | `min(power(x,2)) <= 4`, MAXIMIZE `sum(x)` (today: (7, 7)) |
| composed SUM term | `sum(power(x,2)) + max(x) <= 30` |
| composed objective | `MINIMIZE 2 * max(power(x - t, 2))` with a coupling row |
| flat objective, mixed body | `MAXIMIZE max(power(x,2) + 10*x)` with a budget |
| two-level objective | the two `test_per_objective.py` tests, rebuilt (below) |
| two-level, inner SUM | `MINIMIZE max(per g: sum(power(x - t, 2)) by (g))` with `sum(x) >= B` |
| no box, native off | `DECIDB_NATIVE_CONSTRUCTS=off`: the refusal names `x` |
| HiGHS message | `MINIMIZE max(power(x,2))` (REAL) and the composed push-down query name the MIN/MAX |

`test_maximize_sum_max_power_per` and `test_minimize_sum_min_power_per` get data where the sum-of-squares model
answers differently: MAXIMIZE `sum(per g: max(power(x - t, 2)) by (g))`, targets A (0, 0), B (3), `x` in [0, 10],
`SUM(x) <= 20`, optimum 149 (sum of squares: 109); MINIMIZE `sum(per g: min(power(x - t, 2)) by (g))`, targets
(1, 1 | 1), `SUM(x) >= 15`, optimum 4.5 (sum of squares: 32). Each also solves the sum-of-squares model with
`oracle_solver` and asserts its optimum differs, so the test cannot pass vacuously. Also run
`DECIDB_TEST_DIRECT_SOLVE=off` and `DECIDB_NATIVE_CONSTRUCTS=force`.

**Size.** About 300 lines of C++ (`decide_rewrite_minmax.cpp`, the lift helper beside `decide_rewrite_abs.cpp`'s,
the lift tag through `decide_linear_form.cpp` / `solver_input.hpp`, `decide_solver_gate.cpp`), about 12 tests.

**done.md.** "MIN/MAX — `RewriteMinMax`" and the ABS lift section; `../06_model_formulation/done.md` for the native
arm.
