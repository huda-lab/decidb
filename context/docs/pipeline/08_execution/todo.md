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

Other documented edge differences remain in `done.md` §1a: the D88 variant-name edge, side-plan versus tree error
order for a gated reducer argument, and unreferenced hidden records. Direct-solve rules and DIAGNOSE's separate
named-column scan also bypass or revisit parts of the normal tree execution; their scope is open.

## EXEC-15 — The tree evaluator raises the first error it reaches

- **Problem.** Evaluation catches every error, continues, and raises the least by an eight-key rank
  (`decide_error_ledger.hpp`: phase, step, clause, part, expression, group, position, row). Every node carries rank
  fields; `ErrorRanks`, `WhenRanks` and `KeyedMappingOrder` list cases per node kind; a vector that fails is retried
  row by row and each failing row's error stored. `context/opa_rewrite/repro/error_cost.sql`: one failing `when` over
  200,000 rows takes 3.1 s and 264 MB, where plain SQL fails in 0.02 s. The guide promises no order.
- **Change.** By the owner's ruling (2026-11-18, `context/opa_rewrite/decisions.md` R11): the first error reached is
  raised, as in SQL. A row a `when` drops, or a value no parent reads, raises nothing; nothing else is deferred.
  `DecideBlock`, the rank fields, the rank functions and `Rebase` go, with the tests that pin an order
  (`test_error_precedence.py` and the order cases in other files).
- **Needs.** TEST-15 (the tests the change breaks move to its helper).
- **Done when.** The repro fails in time close to plain SQL's; `grep -rn "DecideBlock\|ErrorRanks" src` finds
  nothing; `./test/decide/run_tests.sh -n 2 -k "error or when or nan"` passes.

## EXEC-16 — Plan- and run-time messages state the kind and where

- **Problem.** The plan- and run-time rows of `language/errors.md` and the data checks of `guide.md` §7.3 that
  OPT-19, OPT-20 and FW-4 leave carry fixes (`Add a constraint like ...`, bounds to add, rewrites to write).
- **Change.** By the owner's rulings (2026-11-18, `context/opa_rewrite/decisions.md` R12, R19): each says what is wrong
  and points at it, and stops. `errors.md` and the guide quote the new text; the opening of `errors.md` states the rule.
- **Needs.** EXEC-15, OPT-19, OPT-20, FW-4; TEST-15 (the tests the rewording breaks move to its helper).
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "error or unbounded or infeasible"` passes.

## EXEC-17 — A refusal that reads only the plan is raised at planning

- **Decision needed.** `x(INT) ... x <= -1` is refused when run: an empty input raises (the guide says it gives an
  empty result) and `EXPLAIN` prints a plan. The check is a 150-line constructor of `DecideGlobalSinkState`.
- **Options.** 1. Raise it at physical planning: `EXPLAIN` and `PREPARE` raise, an empty input raises too. 2. Keep it
  at execution and skip it over an empty input.
- **Recommendation.** 1: the check reads only the plan, so it belongs where the plan is known.
- **Ruling (owner, 2026-11-18).** Option 1: the check moves to physical planning; `EXPLAIN`, `PREPARE` and an
  empty input raise it.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "domain or empty or explain"` passes.

## EXEC-18 — Dead code and stale comments in the operator

- **Problem.** Two timing channels (`DECIDB_BENCH` beside `DecideProfileScope`); dead `Id`, `Ids`, `Mask` in
  `physical_decide.cpp` and a constant `true` member test run per entry; `decide_linear_form.hpp` says
  `decide.prepared` and "last optimizer pass", though it runs at physical planning.
- **Change.** `DecideProfileScope` is the one timing channel; the dead code goes; the comment states where it runs.
- **Done when.** `grep -rn "DECIDB_BENCH" src` finds nothing; `./test/decide/run_tests.sh -n 2 -k "profile"` passes.

## EXEC-19 — An ordinary query builds no diagnosis state

- **Problem.** Every model row carries a `ConstraintProvenance` of about 20 fields, stamped with no `diagnose` gate;
  `plan_decide.cpp` spends about 190 lines harvesting column names whose only reader is the unbounded diagnosis;
  `ParseLiteralCoefficient` recovers a number from a display label with `strtod`.
- **Change.** The stamping and the harvest run under `DIAGNOSE` only; the unit coefficient travels as a number.
- **Done when.** `grep -rn "ParseLiteralCoefficient" src` finds nothing;
  `./test/decide/run_tests.sh -n 2 -k "diagnose or unbounded or infeasible"` passes.

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
