# Code quality

Code-quality issues include duplication, dead code, fragile patterns, unclear
naming, missing coverage, and diagnostic-quality defects that do not change the
ordinary optimization result. Solve-correctness bugs belong in [bugs.md](bugs.md).

Each entry: short title, location (`file:line`), what's wrong, why it matters,
and when/during which task it was discovered.

## CQ-1 — A clause's scope is carried in parallel copies

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
  (D46) are [CQ-3](#cq-3--name-the-keyed-scope-keyed-everywhere).
- **Findings (architect review).** Behaviour can stay the same. Nothing in D47 is serialized, so
  `DECIDB_VERIFY_SERIALIZER=1` cannot catch a regression; `EnumerateExpressions` and the `plan_decide.cpp` column
  harvest can. The location list undercounts: eleven walkers track the enclosing `when`/`per`, not two;
  `ReducerValueLink` and `DecideObjective` hold scope copies too; a helper row is wrapped back two ways
  (`ScopeHelperRow` keeps the `when` only, the MIN/MAX easy half keeps the `per` too, which is
  `../pipeline/05_optimizer/todo.md` § OPT-2's question and stays as is). The once-count part stays with
  [CQ-4](#cq-4--count-once-is-encoded-twice) and `../pipeline/04_canonicalizer/todo.md` § CAN-2.
- **Design.** A borrowed `DecideEnclosingScope` view with one step function `EnterDecideScope` and a read-only leaf
  visitor in `decide_constraint_walk.hpp` (rewrites that replace nodes keep their own recursion and call the step); an
  owned `DecideClauseScope` built from the view, embedded in `DecideConstraint` and `ComposedMinMaxConstraint`
  (Introduce Parameter Object); one exported two-level splitter with three formatters. About 15 files.
- **Decision needed.** Five choices, each with a recommendation.
  1. `group_key` in the struct, as the Work line says, or not? It is read off the comparison, not the wrapper, and the
     composed path can never hold one. Recommended: leave it out.
  2. Under a wrapper, a plain AND starts a fresh scope in four walkers and carries it in five; a `when` inside a `when`
     overwrites in all but direct solve, which asserts it never happens. Nothing builds either shape. One walker
     must pick: fresh scope, or make both shapes refused by the canonical-tree check C0 (with a C++ test)?
     Recommended: refuse in C0.
  3. Field names: `per_key` holds a role, not a key; rename it `per`, `when_condition` becomes `scope.when`?
     Recommended: yes.
  4. Convert the window rewrite and `RewriteInDomain` walkers in this change, direct solve's at
     `../direct_solve/framework/todo.md` § H-07? Recommended: yes.
  5. `DecideObjective` and `ReducerValueLink` stay outside D47 (D46 renames `per_columns`)? Recommended: yes.
- **Order.** Before D46 ([CQ-3](#cq-3--name-the-keyed-scope-keyed-everywhere)), and before
  `../pipeline/04_canonicalizer/todo.md` § CAN-2 if CAN-2 passes the wrapper to every site.
- **Discovered:** 2026-10-18, removing the spellings the scope language replaced
  (D47 in [../language/decisions.md](../language/decisions.md)).

## CQ-2 — How a query is answered is chosen in two places

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
- **Findings (architect review).** The direct rules need the window rewrite's output and not the solver choice;
  `ChooseDecideSolver` needs neither, so one step can hold both without changing which queries go direct.
- **Design.** Strategy selection with fallback (try the direct rules, else choose a solver), the window rewrite as pass
  0 inside `OptimizeDecide`, the policy gate and both `require` errors moved out of `direct_rule_driver.cpp` into the
  step. Making direct solve a solver-registry entry is not recommended: it rewrites the plan, it is not a solver
  session.
- **Decision needed.** Six choices, each with a recommendation.
  1. Choose the backend lazily (only on the solver path) or eagerly? Recommended: lazily; eager makes every first
     direct query probe Gurobi's licence and changes the error for `require` plus an unknown `DECIDB_FORCE_SOLVER`.
  2. `DECIDB_FORCE_SOLVER=""` is read two ways: direct solve treats it as unset, `SelectSolverBackend` throws "is not
     a solver DeciDB knows". One reader must pick one. Recommended: empty means unset, with a test.
  3. What `optimizer_ms` and the `optimizer.decide` span cover: the whole stage on both paths (direct queries start
     printing it), or a separate `direct_ms`? Recommended: the whole stage; update `pipeline_profiler.md` and
     `tpch_suite.md`.
  4. `LogicalDecide::optimized` is documented as "the optimizer started rewriting", which the window rewrite already
     breaks; set it in `ChooseDecideSolver` and document it as "the solver formulation has begun", or rename it
     `formulated`? Recommended: keep the name, fix the doc.
  5. Name and home: `ChooseDecideAnswer` in a new `decide_answer_choice.cpp`, or keep `decide_solver_gate.cpp`?
     Recommended: the new file.
  6. Replace `phase.Next` with a `RunPass(name, fn)` helper in the same change? Two spans time the wrong passes
     (`RewriteNorm` under `optimizer.tag_groups`, `RewriteAbs` under `optimizer.abs` with `TagAbsForBigM`).
     Recommended: yes.
- **Discovered:** 2026-10-27, tracing a query through the code.

## CQ-3 — Name the keyed scope "keyed" everywhere

- **Today:** the language says *keyed*; the code and one label say *entity*: `DecideVarScope::ENTITY`
  (`common/enums/decide.hpp`), `EntityScopeInfo`, `BuildEntityMappings`, `DecideVarScopeInfo::Entity()`/`Keyed()`,
  and DIAGNOSE's `scope` column, which reports `entity` for a keyed decision's escape count. `SCALAR` is the
  query-wide scope; `PER_CONSTRAINT_TAG` and `DecideObjective::per_columns` serve only the two-level objective.
- **Decided (D46):** `DecideVarScope` becomes `ROW / KEYED / GLOBAL` with the values 0/1/2 kept, so serialized plans
  still read; `Entity()` and `Keyed()` merge; `Scalar()` becomes `Global()`; the DIAGNOSE label becomes `keyed`. The
  two-level objective's tag and key field take names that say so.
- **Decision needed.** The names the ruling does not give.
  1. The two-level objective's tag and key field (`PER_CONSTRAINT_TAG`, `DecideObjective::per_columns`): "names that
     say so". Proposed: `TWO_LEVEL_OBJECTIVE_TAG`, `two_level_key`.
  2. The test files (`../testing/todo.md` § TST-2, renamed in the same change): `test_entity_scope.py` and its
     `test_entity_scoped_*` tests → `test_keyed_scope.py` / `test_keyed_*`; `test_scalar_scope.py` →
     `test_global_scope.py`; `test_aggregate_local_when.py` → ?
  3. The `per_clause` and `when*` markers in `pytest.ini`: which names and descriptions replace them?
- **Order.** After D47 ([CQ-1](#cq-1--a-clauses-scope-is-carried-in-parallel-copies)), which removes some renderer
  parameters D46 would otherwise rename.
- **Tests:** `test_query_diagnostics_escaping_instances.py` pins `entity` (update it to `keyed`);
  `DECIDB_VERIFY_SERIALIZER=1` over `test/decide/tests` proves the enum values still round-trip.
- **Docs:** [scopes.md](../language/scopes.md), [diagnose.md](../language/diagnose.md),
  [guide.md §8.1](../language/guide.md#81-what-it-returns), [../diagnostics/](../diagnostics/README.md).

## CQ-4 — "Count once" is encoded twice

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
- **Decision needed.** Which encoding: (1) the builder sums every row and the reduction arrays leave `SolverInput`;
  (2) the builder keeps them and refuses arrays whose sizes differ. Recommended: (1), one fact in one place.
- **Discovered:** 2026-10-27, reviewing the lift of a decision beside MIN/MAX in the objective.
