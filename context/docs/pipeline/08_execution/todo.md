# Stage 08 — Execution: open work

Open work in the tree evaluator and the operator around it. Finished work is in [done.md](done.md). How to work and
the checks: [`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in
these todo files.

## Formulation stays in stage 06

The top-down descent, per-node containers, bottom-up evaluation and frame pass are the deck's (pp. 81-110), and P9's F
nodes own ABS envelopes, Boolean products, L0 indicators and nonlinear reducer definitions. The deck's step 5,
formulation at the nodes (pp. 111-119), is not built, by ruling Q0.4: stage 06 writes the rows, because D89 needs
every reader and the implied bounds (done.md, "Departure from the deck"). Building it means re-ruling D89 so a G can
decide at its node, and replacing `InlineReducerValueLinks`, `EmitExtremumLink` and `LinearizeMinMaxConstraints`
rather than composing records beside them.

Other documented edge differences remain in `done.md` §1a: the D88 variant-name edge, side-plan versus tree error
order for a gated reducer argument, and unreferenced hidden records. Direct-solve rules and DIAGNOSE's separate
named-column scan also bypass or revisit parts of the normal tree execution; their scope is open.

## EXEC-7 — Merge invariants I5 and I6 hold in a release build

- **Problem.** `DecideDescent::Merge` relies on two invariants that only debug assertions check
  (`context/opa_rewrite/review/03_execution_model_and_merge.md`, I5 and I6). I5: classes are matched by a key holding
  thread-local ids, a possible wrong model. I6: a per-class F node under two reducers with different `by` keys can index
  out of bounds. Neither showed at 1 and 8 threads over 500,000 rows (handoff B4, A7 items 6 and 7).
- **Change.** Enforce each as a check that holds in a release build, or find a query that breaks it and fix the merge.
- **Needs.** EXEC-8 (its tests reach both checks).
- **Done when.** EXEC-8's tests reach both checks; `./test/decide/run_tests.sh -n 2 -k descent_merge` passes.

## EXEC-8 — Real-thread tests for the descent merge

- **Problem.** No suite test runs the tree on more than one thread. The four tests of `test_descent_merge.py` set
  `threads = 1` and simulate threads by merging every n rows. The thread check of the merged branch ran outside the
  suite (`context/opa_rewrite/tools/threads_test.py`: 8 shapes, 1 and 8 threads).
- **Change.** Oracle tests over several row groups on real threads replace the four; `threads_test.py`'s shapes are
  the starting set. DuckDB hands threads whole row groups (122,880 rows), so each test needs more than one.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k descent_merge` passes, and each test shows more than one
  thread's descent merged (a profile counter or EXPLAIN ANALYZE).

## EXEC-9 — The 23 failures under the descent-split switch

- **Problem.** OPA's last commit message reports 23 suite failures with the descent split after every row
  (`SET decide_debug = 'split=1'`). Nobody has sorted them (handoff § A4, problem 5).
- **Change.** Run the suite once with the split on and file each failure: a defect as its own task
  in the todo of the stage that owns it, with its query and a focused test; a failure that reads a thread-dependent
  detail as a **Decision needed** line here, with its evidence.
- **Done when.** Each of the failures has a task or a **Decision needed** line; the fixes are those tasks.

## EXEC-10 — A collation error in a frame's order key names the wrong row

- **Problem.** The frame order-key comparison in `decide_descent.cpp` passes positions `0..n-1` instead of the rows'
  own positions, so a collation error names the wrong row (handoff A7, item 8; `done.md` § 1a, D12). The default
  build's collations (`nfc`, `noaccent`, `nocase`) cannot raise on valid UTF-8, so no SQL reaches the error in that
  build.
- **Change.** Pass the rows' positions. Check whether the ICU extension loads here and has a collation that raises on
  some input.
- **Done when.** With such a collation, a test through it names the right row. With none, this task records the evidence
  (the collations tried and what each did) and a **Decision needed** line: keep the branch untested, or delete it as
  unreachable.

## EXEC-12 — "Takes more than one value" under `per K:` names no group and can pair two classes' values

- **Problem.** Seen on 052b85d26d with `SET decide_direct_solve = 'off'`. Over `t(id, g, cap)` = (1, 'a', 3), (2, 'a',
  4), (3, 'b', 2), with `x(INT)`, `x >= 0 AND x <= 10 AND per g: sum(x) = t.cap` and `MAXIMIZE SUM(x)`, the error is
  "`t.cap` takes more than one value here (2 and 4) — an `=` bound cannot hold two different values at once, so this
  DECIDE query is infeasible." It names no group, and 2 and 4 come from classes b and a; the conflict is inside class a
  (3 and 4). `sum(x) by (g) = t.cap` gives "... (3 and 4), in the group g = 'a' ...".
- **Cause.** The check of 9235fbc18e groups by the clause's `by` key and not by its `per` class:
  `decide_value_extraction.cpp` (the solver path) and `DirectGroupKey` in `direct_plan_fragments.cpp`. Not traced
  further.
- **Change.** The check groups by the instance, its `per` class and its `by` key, on both paths, and names the group as
  `DescribeKeyValues` writes it.
- **Done when.** A test of this query names g = 'a' and (3 and 4) under `off` and `auto`, and an oracle test answers
  `per g: sum(x) = t.cap` with one cap per class; `./test/decide/run_tests.sh -n 2 -k "<the new tests> or
  bound_side_tables or error_precedence"` passes.

## EXEC-13 — done.md explains the tree by the deleted flat evaluator

- **Problem.** `done.md` lines 98-461 describe the tree evaluator by comparison with the deleted flat evaluator, with
  dates and history, where the docs state the current system and its reasons.
- **Change.** Each passage states the tree's rule in its own terms, as the comments do; dates and history
  go. "Flat" meaning a non-nested objective stays.
- **Done when.** Those sections name the flat evaluator nowhere and hold no dates.

## EXEC-14 — Finalize's comments number phases the code does not have

- **Problem.** `physical_decide.cpp` and `physical_decide.hpp` label `Finalize`'s steps "PHASE 1.6", "PHASE 2,
  sub-phase 3", "PHASE 3a" and so on (15 places). The numbering is the flat evaluator's: "PHASE 1.6 -- scope checks"
  heads code that checks no scope, and PHASE 2 is `EvaluateTree`.
- **Change.** Each banner and comment names the step by what its code does (`EvaluateTree`, `BuildSolverInput`,
  `FormulateModel`, `FinalizeSolveResult`), with no number; a banner over no step goes.
- **Done when.** `grep -rn "PHASE [0-9]" src` finds nothing; the diff changes comments only; `make release` builds.
