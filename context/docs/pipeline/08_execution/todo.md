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

## EXEC-3 — Delete the dead evaluator code

- **Problem.** `context/opa_rewrite/review/07_dead_code_opa.md` §§ 1–3 list code nothing reads; its line numbers are
  stale, so find each by name. Present on `OPA-rewrites`: `DecideEvaluationFields`, both `ExecuteOnReadRows`,
  `DecideListRowMembers`, `AppendPayloadLeaves`, `DecideFrameMap::PeerSize`, `VerifyCountedOnce` (its
  `row_group_ids` is always empty), `DecideGlobalSinkState::canonicalizer` and `context`, `frame_column_refs`; the
  write-only fields `repair_rows`, `minmax_easy`, `tightest_bound`, `child_reads` with `DecideTreeRead`,
  `rows_stand_with_their_class` and `ComposedMinMaxTerm::Kind`; and parameters every caller passes the same value:
  `EvaluateTree`'s `silent` and `descent`, `FormulateElasticModel`'s `keep_singleton_rows`, `DescentHash`'s `parent`,
  `DescentRowValues`'s `begin`, `CollectRangeSigns`'s `sign`, `TakeTightestFrameBound`'s `grouped`, `AssignDense`'s
  `init`. About 200 lines.
- **Change.** Delete them. Keep what the debug dumps read (`DecideNode::hidden`, `l0_auto_m`; EXEC-4 keeps the dumps)
  and `DecideListMember`, which is live.
- **Done when.** `git grep` finds none of the deleted names; `build/release/test/unittest "[decidb]"` and
  `./test/decide/run_tests.sh -n 2 -k "one_pass or descent or frames"` pass.

## EXEC-4 — The debug dumps sit behind one setting read once per query

- **Problem.** The evaluator's debug output and test hooks read nine environment variables. `DECIDB_DESCENT_CHUNK` and
  `DECIDB_DUMP_STEPS` are read on every Sink chunk (`AppendDecideDescent`, `physical_decide.cpp`);
  `DECIDB_DESCENT_SPLIT` per thread state; `DECIDB_DUMP_BODIES`, `DECIDB_DUMP_TREE`, `DECIDB_DUMP_CONTAINERS`,
  `DECIDB_DUMP_VALUES` and `DECIDB_TREE_INJECT` once per query; `DECIDB_EVALUATOR_INJECT` once per process
  (`decide_tree.cpp`). `DECIDB_DUMP_VALUES` has no test.
- **Change.** By the owner's ruling (handoff § A6, item 11), the dumps stay as a debugging tool behind one setting,
  read once per query and passed down. DuckDB's `SET` options are the pattern `decide_direct_solve` follows. The tests
  that use these variables switch to the setting. `DECIDB_BENCH`, `DECIDB_STATUS_MARKERS` and the solver variables
  are outside this task.
- **Ruling (owner, 2026-11-17).** The setting is one option, `decide_debug`, a comma list: `tree`, `bodies`,
  `containers`, `values` and `steps` print their dumps, `chunk=N` and `split=N` set the descent's chunk and split, and
  `inject=N:data:V` is `DECIDB_TREE_INJECT`'s hook. It is registered as `decide_direct_solve` is, with a set callback
  that rejects a bad item at `SET`; physical planning reads it once into a struct the operator carries.
  `DECIDB_EVALUATOR_INJECT` goes with its tests when no kept test uses it. `values` gets a test.
- **Needs.** TEST-3 (it removes most of the tests that read the dumps).
- **Done when.** No `getenv` of these names remains outside the one reader;
  `./test/decide/run_tests.sh -n 2 -k "descent_merge or one_pass or null_values or tree_bodies"` passes.

## EXEC-5 — The descent outlives the evaluation

- **Problem.** `gstate.descent` (`decide_global_sink_state.hpp`) keeps every node container alive through the solve
  and the output until the global sink state is destroyed. Nothing reads it after `EvaluateTree`
  (`physical_decide.cpp`); the readers that outlive it copy their data.
- **Change.** Release it when `EvaluateTree` returns.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "one_pass or descent or frames"` passes.

## EXEC-6 — Comments explain the evaluator by the deleted flat evaluator

- **Problem.** 106 comment lines explain behaviour by reference to the deleted flat evaluator ("as flat numbers
  them", "flat's order"), most in `decide_tree_compile.cpp`, `decide_model_emitter.{hpp,cpp}`, `decide_tree.{hpp,cpp}`,
  `decide_descent.cpp`, `decide_tree_columns.{hpp,cpp}` and `decide_error_ledger.hpp`. `physical_decide.cpp` has three
  PHASE banners with no code under them, and "transition" or "shadow" wording describes a migration with nothing left to
  migrate (`logical_decide.hpp`, `decide_tree.cpp`, `physical_decide.hpp`, `enums/decide.hpp`).
- **Change.** Each comment states its rule in its own terms; empty banners go. "Flat" meaning a non-nested objective
  stays.
- **Done when.** Comment-only diff; `make release` builds and `make format-check` reports only `logical_operator.hpp`.

## EXEC-7 — Merge invariants I5 and I6 hold in a release build

- **Problem.** `DecideDescent::Merge` relies on two invariants that only debug assertions check
  (`context/opa_rewrite/review/03_execution_model_and_merge.md`, I5 and I6). I5: classes are matched by a key holding
  thread-local ids, a possible wrong model. I6: a per-class F node under two reducers with different `by` keys can index
  out of bounds. Neither showed at 1 and 8 threads over 500,000 rows (handoff B4, A7 items 6 and 7).
- **Change.** Enforce each as a check that holds in a release build, or find a query that breaks it and fix the merge.
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

- **Problem.** OPA's last commit message reports 23 suite failures under `DECIDB_DESCENT_SPLIT=1`. Nobody has sorted
  them (handoff § A4, problem 5).
- **Change.** Run the suite with the split on, then fix each failure that is a defect, and bring the owner, with
  evidence, each one that reads a thread-dependent detail.
- **Needs.** EXEC-4 (the switch takes the one setting's spelling).
- **Done when.** Every failure is a fix with a test or an owner decision; the split run of the affected files passes.

## EXEC-10 — A collation error in a frame's order key names the wrong row

- **Problem.** The frame order-key comparison in `decide_descent.cpp` passes positions `0..n-1` instead of the rows'
  own positions, so a collation error names the wrong row (handoff A7, item 8; `done.md` § 1a, D12). The default
  build's collations (`nfc`, `noaccent`, `nocase`) cannot raise on valid UTF-8, so no SQL reaches the error in that
  build.
- **Change.** Pass the rows' positions. Check whether the ICU extension loads here and has a collation that raises;
  with one, test the fix through it. With none, the error branch is unreachable: bring the owner that evidence.
- **Done when.** A test through a raising collation names the right row, or the owner's decision is recorded.
