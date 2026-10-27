# Code quality

Code-quality issues include duplication, dead code, fragile patterns, unclear
naming, missing coverage, and diagnostic-quality defects that do not change the
ordinary optimization result. Solve-correctness bugs belong in [bugs.md](bugs.md).

Each entry: short title, location (`file:line`), what's wrong, why it matters,
and when/during which task it was discovered.

## A clause's scope is carried in parallel copies

- **Location:** `src/include/duckdb/planner/decide/decide_prepared_model.hpp`
  (`DecideConstraint`: `when_condition`, `group_key`, `per_key`, `class_key`) and
  `src/include/duckdb/planner/operator/decide/logical_decide.hpp`
  (`ComposedMinMaxConstraint`: `when`, `per_key`, `class_key`); the walkers
  `EnterClauseScope` (`decide_optimizer.cpp`, used by the ABS, norm/IN and value
  rewrites) and the canonicalizer's `DecideEnclosingScope`; the two-level objective's
  text, built by `RenderTwoLevelObjective` / `RenderTwoLevelInner`
  (`decide_source_provenance.cpp`) and again for the account subjects in
  `decide_rewrite_minmax.cpp` (`RenderDecidePerKey`); the rule that a query-wide
  decision beside the reducers counts once, applied by hand in `ilp_model_builder.cpp`
  (linear and quadratic paths) and `linearization_not_equal.cpp`, beside execution's
  `CountOnce` (`physical_decide.cpp`).
- **What's wrong:** one concept, several owners. A clause's `when` / `per K:` / `by`
  scope is declared on two structs and filled by two readers; the enclosing scope is
  walked by two walkers; the two-level form is rendered twice; the once-count rule has
  several hand copies.
- **Why it matters:** a change to how a clause is scoped, rendered or counted must find
  every copy; a missed one gives a wrong count or an echo that differs between EXPLAIN
  and the rewrite accounts.
- **Work (D47):** one `DecideClauseScope { when; per_key; class_key; group_key; }`
  embedded in both structs, one enclosing-scope walker in `decide_constraint_walk.hpp`,
  one exported two-level renderer, and one owner of the once-count rule at execution.
  Behaviour does not change: the scope suites, the golden corpus (identical on both
  solvers) and `DECIDB_VERIFY_SERIALIZER=1` stay green. The renames that go with it
  (D46) are [Name the keyed scope "keyed" everywhere](#name-the-keyed-scope-keyed-everywhere).
- **Discovered:** 2026-10-18, removing the spellings the scope language replaced
  (D47 in [../language/decisions.md](../language/decisions.md)).

## "A bound on one decision" is classified three times

- **Location:** `FoldSingletonRows` / `PeelAffine` (`decide_singleton_row_folding.cpp`),
  `TryFoldPerRowBound` (`ilp_model_builder.cpp`, around line 702), `DirectReadBounds` /
  `DirectClauseBounds` (`src/optimizer/decide/direct/direct_bound_reader.cpp`).
- **What's wrong:** the optimizer folds a literal bound in any affine spelling
  (`2 * x >= -4`); the model builder folds a data bound only when the emitted row has one
  index with coefficient ±1 (`x <= cap`, not `2 * x <= cap`), deciding after it has built
  the row and its provenance; direct solve reads any coefficient per row and rounds for an
  integer decision, but declines a bound the optimizer folded in an affine spelling. Three
  stages, three shape tests, three reaches.
- **Why it matters:** whether a bound is a column bound or a row depends on which stage
  looks; the HiGHS QP speedup from folding misses the scaled spellings; direct solve
  declines queries the solver folds.
- **Work:** one classifier that decides at plan time (the optimizer, where the backend
  choice is also made once) and tags the comparison with its coefficient and side; the model
  builder and direct solve read the tag, with one rounding rule
  (`decide_numeric_policy.hpp`). It covers the direct todo "Read a folded affine bound"
  (A1 and S1) and the solver-path fold below.
- **Solver-path fold of a scaled per-row bound:** `TryFoldPerRowBound`
  (`ilp_model_builder.cpp`) folds a data-valued bound on one decision only when its
  coefficient is 1 or -1. `2 * reserve <= 2 * (D.capacity - 5)` stays one row per class,
  while `reserve <= D.capacity - 5` is the column's box. The owner wants every bound on one
  decision to fold, so the scope tests assert the box. Divide the right side by the
  coefficient, flip the sense on a negative one, and round the quotient for a
  whole-number decision with the same rule as the direct reading (the boundary of
  `int_upper_quotient_just_below_a_whole_number`). Test:
  `test_a_scaled_bound_on_one_decision_folds_like_the_plain_one` in
  `test_scope_generation.py` is `xfail(strict=True)`; remove the mark. Add a golden corpus
  case with a negative coefficient and one whose quotient is not a whole number. Record the
  result in `../pipeline/06_model_formulation/done.md`.
- **Discovered:** 2026-10-27, tracing a query through the code.

## How a query is answered is chosen in two places

- **Location:** `DecideOptimizer::OptimizeSubtree` (`decide_optimizer.cpp`, around line 61:
  `RewriteRhsAggregates`, then `ApplyDirectSolveRules`) and `ChooseDecideSolver`, called
  once in `OptimizeDecide`.
- **What's wrong:** `OptimizeDecide`'s comment says the backend is "settled only once",
  before any rewrite; but direct solve versus the solver is settled earlier, outside it,
  after a rewrite (the window) that sits outside the pass list, its profiling scope and
  `DECIDB_BENCH` timing.
- **Why it matters:** a reader looking for "where is the backend chosen" finds the comment
  that says one place and code that uses two.
- **Work:** one choice step that picks among direct solve, Gurobi and HiGHS; the window
  rewrite becomes pass 0 of the listed passes. The physical-planning fallback that called
  `ChooseDecideSolver` a second time is gone: a plan the optimizer never saw is refused
  (`../pipeline/05_optimizer/done.md`).
- **Discovered:** 2026-10-27, tracing a query through the code.

## Name the keyed scope "keyed" everywhere

- **Today:** the language says *keyed*; the code and one label say *entity*: `DecideVarScope::ENTITY`
  (`common/enums/decide.hpp`), `EntityScopeInfo`, `BuildEntityMappings`, `DecideVarScopeInfo::Entity()`/`Keyed()`,
  and DIAGNOSE's `scope` column, which reports `entity` for a keyed decision's escape count. `SCALAR` is the
  query-wide scope; `PER_CONSTRAINT_TAG` and `DecideObjective::per_columns` serve only the two-level objective.
- **Decided (D46):** `DecideVarScope` becomes `ROW / KEYED / GLOBAL` with the values 0/1/2 kept, so serialized plans
  still read; `Entity()` and `Keyed()` merge; `Scalar()` becomes `Global()`; the DIAGNOSE label becomes `keyed`. The
  two-level objective's tag and key field take names that say so.
- **Tests:** `test_query_diagnostics_escaping_instances.py` pins `entity` (update it to `keyed`);
  `DECIDB_VERIFY_SERIALIZER=1` over `test/decide/tests` proves the enum values still round-trip.
- **Docs:** [scopes.md](../language/scopes.md), [diagnose.md](../language/diagnose.md),
  [guide.md §8.1](../language/guide.md#81-what-it-returns), [../diagnostics/](../diagnostics/README.md).

## "Count once" is encoded twice

- **Location:** `CountTupleTermsOnce` (`physical_decide.cpp`) and the model builder's `is_reduced_scalar`
  shortcut (`ilp_model_builder.cpp`, the objective loop; the constraint loops use the same pattern), read from
  `SolverInput::objective_term_reductions` and the constraint-side `linear_term_reductions`.
- **What's wrong:** stage 08 already zeroes every row but one of a term that is counted once. The model builder
  encodes the same fact again, from a parallel array, and reads a missing entry as "once, from row 0".
- **Why it matters:** a writer that appends coefficient columns without their reduction entry gets a wrong answer
  and no error. The composed MIN/MAX objective did this (`MAXIMIZE max(x) + sum(s * cap)` counted `s * cap` on
  one row); that writer is gone, but the trap is not.
- **Work:** one encoding. Either the builder sums every row (the rows not counted are already 0) and the
  reduction arrays leave `SolverInput`, or the builder keeps them and refuses arrays whose sizes differ. Check the
  zero-row fallback (`col.Size() == 0 ? 1.0`) first.
- **Discovered:** 2026-10-27, reviewing the lift of a decision beside MIN/MAX in the objective.
