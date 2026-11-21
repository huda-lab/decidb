# Stage 08 — Execution: open work

Open work in the tree evaluator and the operator around it. Finished work is in [done.md](done.md). How to work and
the checks: [`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in
these todo files.

## Formulation at the nodes is Phase 6

The top-down descent, per-node containers, bottom-up evaluation and frame pass are the deck's (pp. 81-110), and P9's F
nodes own ABS envelopes, Boolean products, L0 indicators and nonlinear reducer definitions. The deck's step 5,
formulation at the nodes (pp. 111-119), is not built: stage 06 writes the rows, because D89 needs every reader and
the implied bounds (done.md, "Departure from the deck"). Phase 6 builds it (ARCH-2, [`../todo.md`](../todo.md)):
that means re-ruling D89 so a G can decide at its node, and replacing `InlineReducerValueLinks` and `EmitExtremumLink`
rather than composing records beside them.

Other documented edge differences remain in `done.md` §1a: the D88 variant-name edge and unreferenced hidden
records. Direct-solve rules and DIAGNOSE's separate
named-column scan also bypass or revisit parts of the normal tree execution; their scope is open.

## EXEC-24 — A folded bound over a decision with no instance reports as any clause does

- **Problem.** A clause left with no instance warns that it constrains nothing (`ReportSkippedInstances`), but a
  clause stage 05 folds into the box never reaches the row evaluation: `DecideModelEmitter` re-reads a folded bound
  only when it reads a helper. With no row in a class, `per k: c BETWEEN 0 AND 10` says nothing while `c + d <= 5`
  warns. `DecideAbsence::MayBe` is true for every keyed decision, so re-reading every folded keyed bound would add a
  per-row pass to queries whose decisions have an instance on every row.
- **Change.** The folded-bound read hands every term to `DecideAbsentReads`, as the row paths do, at no cost where
  every row has a class: `MayBe` answers exactly once the classes are read, or the instances are counted where the
  box fold reads the bound's data.
- **Ruling (owner, 2026-11-21).** One null rule, folded bounds included. The solver path of
  `test_a2_keyed_every_row_with_no_class_has_no_instance` gains the warning; the direct path stays as it is.
- **Done when.** The repro warns on the solver path; a 100,000-row keyed bound with every row in a class takes no
  longer than before; `./test/decide/run_tests.sh -k "constrains_nothing or no_instance or a2_keyed"` passes.

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
  singleton-row boxes and `tree.decide_index`.
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
