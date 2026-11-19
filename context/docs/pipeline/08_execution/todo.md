# Stage 08 — Execution: open work

Open work in the tree evaluator and the operator around it. Finished work is in [done.md](done.md). How to work and
the checks: [`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in
these todo files.

## Formulation at the nodes is Phase 6

The top-down descent, per-node containers, bottom-up evaluation and frame pass are the deck's (pp. 81-110), and P9's F
nodes own ABS envelopes, Boolean products, L0 indicators and nonlinear reducer definitions. The deck's step 5,
formulation at the nodes (pp. 111-119), is not built: stage 06 writes the rows, because D89 needs every reader and
the implied bounds (done.md, "Departure from the deck"). Phase 6 builds it (ARCH-2, [`../todo.md`](../todo.md)):
that means re-ruling D89 so a G can decide at its node, and replacing `InlineReducerValueLinks`, `EmitExtremumLink`
and `LinearizeMinMaxConstraints` rather than composing records beside them.

Other documented edge differences remain in `done.md` §1a: the D88 variant-name edge and unreferenced hidden
records. Direct-solve rules and DIAGNOSE's separate
named-column scan also bypass or revisit parts of the normal tree execution; their scope is open.

## EXEC-19 — A model row carries a back-pointer, and the diagnosis derives the rest

- **Problem.** `SolverInput::diagnose` gates the display fields of `ConstraintProvenance` and the harvest of input
  column names in `plan_decide.cpp` (`NameInputColumns`). Each emission site carries a `diagnose` branch, every helper
  that stamps a row has one, and the display fields are written where the row is built.
- **Change.** **Ruling (owner, 2026-11-19, Decision 5).** Late materialization. `ConstraintProvenance` keeps a
  back-pointer to the row's clause, always: `repair_group_id`, `group_key` and `data_row`, with no new field. One
  `ResolveProvenance` in the diagnosis layer derives kind, shape, labels, class set, `per_kept_row` and `is_aggregate`
  from it under `DIAGNOSE`, through a resolver callback in `InfeasibleDiagnosisInput` (as `solve_model` is), in
  `src/execution/operator/decide/decide_provenance_resolver.cpp`; about 15 signatures and two C++ test files change.
  `strict`, `typed_k` and `rhs_mechanism_offset` stay on the row as formulation facts, and `lhs_offset` goes. The stamp
  helpers lose their `diagnose` branches. `keep_singleton_rows` is restored as the name of the singleton-row mode, and
  the `<>` indicator labels become a numeric back-pointer. `NameInputColumns` moves into the unbounded engine, called
  from `plan_decide` under `DIAGNOSE` only, and `composed_minmax_constraints` moves with it. `DIAGNOSE` output stays
  byte-identical: the 18 examples and the goldens.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "diagnose or unbounded or infeasible"` passes and a golden shows no
  change; `build/release/test/unittest "[decidb]"` passes.

## EXEC-20 — SolverInput is the whole contract of model building

- **Problem.** `physical_decide.cpp` is 2,132 lines, about 425 of them the operator. `FormulateModel`, a second
  hand-ordered pass list, reads operator fields beside `SolverInput`.
- **Change.** The emitter writes everything model building reads into `SolverInput`; `FormulateModel(input, box)`
  moves to `src/decidb/formulation` and stays a pure function, so the diagnosis re-runs it against a widened box.
- **Done when.** `FormulateModel` reads no operator field; `./test/decide/run_tests.sh -n 2 -k "diagnose or
  formulation"` passes.

## EXEC-21 — Solve and route live beside the solver

- **Problem.** `FinalizeSolveResult` is 479 lines in the operator file. `SolverModel::Build` is one function of 555
  lines, complexity 119.
- **Change.** Solving, status handling and routing to readback move beside the solver facade; the operator calls
  them. `Build` is split into columns, objective, clause rows and raw rows.
- **Needs.** EXEC-20.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "status or time_limit or infeasible"` passes.

## EXEC-22 — A plan fact has one home

- **Problem.** `plan_decide.cpp` copies 35 fields by hand from `LogicalDecide` to `PhysicalDecide`; frames,
  signatures, keys and scopes are read from both the operator and the tree. Dead at execution: the tree's copy of the
  singleton-row boxes, `tree.decide_index`, `PhysicalDecide::scope_checks`.
- **Change.** The tree owns each fact; the operator keeps the child, the tree, the debug options and one display
  payload; the name harvest uses the tree's own enumeration of row expressions.
- **Needs.** EXEC-20.
- **Done when.** `DECIDB_VERIFY_SERIALIZER=1 ./test/decide/run_tests.sh -n 2 -k "frame or per or explain"` passes.

## EXEC-23 — The thread model of the descent

- **Decision needed.** Each thread routes its rows as they arrive and the routes are merged: `DecideDescent::Merge`
  is 316 lines, complexity 69, with two run-time guards for shapes that cannot merge. It buys about 0.1 s of 1.3 s
  on 2 million rows.
- **Options.** 1. One route over the buffered rows in `Finalize` ("one scan of the buffer"); the merge goes.
  2. Keep routing per thread ("reads each row as it arrives").
- **Recommendation.** 1: the merge costs more code than its time saves.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "descent or merge or thread"` passes; `tools/ab_bench.py`
  shows no workload moving beyond about 10%.

## EXEC-25 — A failing coefficient is found at the first entry

- **Problem.** A coefficient that fails on one row over 200,000 rows takes 4.6 s: `ComputeNode` evaluates every
  entry up front, and `DecideEvaluateWindow` then reads the failing window row by row. Plain SQL fails in milliseconds.
  The repro is `context/opa_rewrite/repro/failing_coefficient.sql`.
- **Decision needed.** Where the first failing entry is found: 1. Compute in windows and stop at the first window that
  fails. 2. Keep the whole-column computation and shorten the retry.
- **Recommendation.** 1: the guide promises no order among errors, so the first failing window may stop the query.
- **Done when.** The repro fails in time close to plain SQL's; `./test/decide/run_tests.sh -n 2 -k "nan or finite or
  coefficient"` passes.
