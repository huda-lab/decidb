<!-- Full report of a review agent, saved 2026-11-16. Model output, not verified unless the main report (../handoff.md) marks a finding as re-checked. -->

# ANR (flat) vs OPA (tree): structural review

Read-only review. Nothing was built, run or edited; the only tool run was `lizard` on source files. Paths use two absolute roots:

- `$ANR` = `/Users/hatim/Code/decidb/.claude/worktrees/sami-anr` (300e387bb1)
- `$OPA` = `/Users/hatim/Code/decidb/.claude/worktrees/sami-opa` (7837e300be)

## Structural verdict

OPA is the only branch with the deck's steps 1-4 as real structures (run-time nodes, per-node containers filled in `Sink`, bottom-up data values), so it is the sounder base for step 5. As it stands it is not a clean one. It is about twice the size of ANR's evaluator, and its tree is not the source of truth for the model: stage 05's flat term records are, with a second "algebra" mirror checked against them. Its emitter scatters node values back into the same row-indexed structures ANR produces. Its error ordering is specified by reference to the deleted flat evaluator. ANR is smaller and closer to a plain DuckDB blocking operator, but it has no node identity at run time, and its core is one 654-line function. I recommend keeping OPA, with three paydowns before step 5 starts (proposals P1-P3). The call is yours.

## Q1. Actual design and pattern fit

### ANR

- **Data flow.** `Sink` appends to a thread-local `ColumnDataCollection`; `Combine` merges under a lock (`$ANR/src/execution/operator/decide/physical_decide.cpp:2108-2124`). `Finalize` does everything else on one thread (`:3017-3113`).
- **Evaluation.** `RegisterEvaluation` lists every expression any later phase reads (`:2448-2598`). `DecideRowStore::Build` evaluates all of them on every row in one scan (`$ANR/src/execution/operator/decide/decide_row_store.cpp:166-204`). Fixed phases then read the store: frames, keyed mappings, scope checks, constraints, objective, composed, value links (`physical_decide.cpp:3115-3174`).
- **Contract.** Stage 05 hands stage 08 one struct of flat term lists, `DecidePreparedModel` (`$ANR/src/include/duckdb/planner/decide/decide_prepared_model.hpp:308-330`).
- **Pattern.** Materialize-then-compute pipeline breaker plus a table-driven interpreter. It matches DuckDB's simplest sink shape. It departs from DuckDB by doing all compute single-threaded in `Finalize`, and by evaluating expressions on rows a clause later drops, which forces per-row error deferral (`decide_row_store.hpp:17-20`, `decide_row_store.cpp:43-83`).
- **Against the deck.** "One pass" here means one scan. There is no annotated tree at run time ("the tree is the operator's tables", `physical_decide.cpp:2444-2447`), no routing, and no per-node containers.
- **One elegant piece.** A frame's values become columns appended after the input's, so no reader learns a new case (`decide_prepared_model.hpp:246-252`, `decide_row_store.cpp:206-211`).

### OPA

- **Data flow.** Each thread's `Sink` buffers the chunk and runs a `DecideDescent` over it (`$OPA/src/execution/operator/decide/physical_decide.cpp:409-450`). The descent evaluates `when`, keys, signatures and carried columns only on rows reaching each node (`$OPA/src/execution/operator/decide/decide_descent.cpp:1613-1666`). `Combine` parks the thread's descent with its row offset (`physical_decide.cpp:380-395`).
- **Finalize.** `MergeAll` merges the dictionaries serially, then copies entries in parallel tasks (`decide_descent.cpp:1762-2195`). `DecideTreeColumns` computes each node's "programs" over its entries, lazily and memoized (`$OPA/src/execution/operator/decide/decide_tree_columns.cpp:157-290`). `DecideModelEmitter` writes `EvaluatedConstraint`, `ReducerValueLinkSpec` and the rest for the unchanged stage 06 (`physical_decide.cpp:831-891`).
- **Tree construction.** Built at physical planning from the post-rewrite clauses; `LowerDecideTree` then writes the flat records into the roots (`$OPA/src/execution/physical_plan/plan_decide.cpp:64-73`, `$OPA/src/optimizer/decide/decide_linear_form.cpp:119-235`).
- **Pattern.** Per-thread hash grouping with a merge, as a hash aggregate does. `DecideKeyMap` reuses `GroupedAggregateHashTable` (`$OPA/src/execution/operator/decide/decide_key_map.cpp:33-63`), and `DecideAggregateStates` calls `AggregateFunction` directly (`$OPA/src/include/duckdb/execution/operator/decide/decide_tree_columns.hpp:27-56`). The descent is vectorized by selection.
- **Departures from DuckDB.**
  - The merge runs inside `Finalize` with a blocking `TaskExecutor`, not a scheduled finalize event (`decide_descent.cpp:2115-2179`).
  - `DecideColumns` re-implements a chunked column store in unmanaged memory (`$OPA/src/include/duckdb/execution/operator/decide/decide_descent.hpp:40-76`).
  - Many paths go through `Value` and `vector<bool>(num_rows)` rather than vectors.
- **Departures from the named patterns.** "Composite" is declared (`$OPA/src/include/duckdb/planner/decide/decide_tree.hpp:164`), but `DecideNode` is one struct with a kind enum and 66 members, many valid for one kind only (`:168-290`). There is no visitor: the bottom-up half is per-construct emitter methods called in the flat evaluator's phase order (`physical_decide.cpp:873-885`).
- **Against the deck.** Steps 1, 2 and 4 are real. Step 3 is real for data values only; a decision-valued G is still a plan-time helper variable (`$OPA/src/planner/decide/decide_tree.cpp:1960-2018`).

## Q2. Structural quality

| Axis | ANR | OPA |
|---|---|---|
| Size (lizard NLOC, evaluator files) | about 6.4k | about 12.2k; `git diff --stat` is +16131 / -7257 |
| Largest function | `EvaluateConstraints`, 654 NLOC, CCN 180 (`physical_decide.cpp:3903-4687`) | `DecideTree::Validate`, 523 NLOC, CCN 347 (`decide_tree.cpp:2359-2889`); `DecideDescent::Merge`, 309 NLOC, CCN 66 (`decide_descent.cpp:1762-2085`) |
| File split | One 6787-line file | Split by responsibility: descent, columns, emitter, extraction, absence, key map |

### ANR findings

- **Growing case list.** `RegisterEvaluation` must mirror every reader by hand. A missed registration is an internal error at read time (`decide_row_store.cpp:235-245`).
- **Flag-driven monolith.** `EvaluateConstraints` branches on about 35 distinct `DecideConstraint` fields.
- **Two-phase fix-ups.** Group numbers are unstable until `ResolveGroupNames` (`physical_decide.cpp:2060-2085`). The no-value rule is spread over about ten statics (`:2673-2785`, `:3734-3900`).
- **Vestigial code.** The evaluator switch and verify harness exist for a tree that is not in this branch. `VERIFY` compares flat with itself, and `TREE` throws "not built yet" (`:3059-3070`, `:3177-3409`, `$ANR/src/include/duckdb/execution/operator/decide/decide_evaluator.hpp:6-25`). That is about 420 lines.
- **Good.** One stage 05 to 08 contract. Equal expressions are evaluated once across clauses (`decide_row_store.cpp:90-102`).

### OPA findings

- **The same clause is held three times.** The flat records (`DecideBody::clause`, `decide_tree.hpp:123-162`), the programs and sites, and `DecideAlgebraNode`, which is built from the flat records (`decide_tree.cpp:915-964`). `Validate` checks the algebra term by term against the records (`:2452-2508`), and the emitter checks again on every read (`$OPA/src/execution/operator/decide/decide_model_emitter.cpp:28-310`). The algebra is stringly typed: `"+"`, `"*"`, `"composed_sum"`, `"frame:"`, `"minimize"`.
- **Round trips.** The objective sense is on the operator, copied to `tree->objective_sense` (`decide_tree.cpp:178`), written into the algebra as a string, and parsed back (`physical_decide.cpp:69-94`, used at `:1330`). `flat_objective_agg` is read at `:288`, while `:1373` and `:1610` re-derive it from the algebra. Keys, scopes and boxes are copied from the operator into the tree (`decide_tree.cpp:178-186`).
- **Behaviour defined by deleted code.** `DecideErrorPhase`, `DecideBlock` and the 8-field `DecideErrorRank` encode the flat evaluator's control flow (`$OPA/src/include/duckdb/common/decide_error_ledger.hpp:18-68`). Every node carries four ranks plus `key_block`, `filter_group` and `filter_part` (`decide_tree.hpp:179-190`, `:266-275`); every program carries `block`, `group`, `position` and `part` (`:108-116`). `ErrorRanks` is a per-node-kind case list (`decide_tree.cpp:1277-1363`). About 150 lines across seven files mention this machinery, and about 100 comments say "as flat does".
- **The emitter is a tree-to-flat adapter.** It keeps ANR's dispatch (`clause.lhs_is_aggregate || clause.was_minmax_easy`, `decide_model_emitter.cpp:886-890`) and scatters entry values into dense per-row vectors (`:951-956`, `:1394-1423`).
- **Layer leaks.**
  - The planner's tree type includes execution error ranks.
  - A `DecideTreeConstruction` method is defined in the optimizer (`$OPA/src/optimizer/decide/decide_tree_definition.cpp:228`).
  - Tree node ids reach stage 06 as `value_nodes`, whose only present effect is a consistency exception (`$OPA/src/decidb/formulation/linearization_reducer_value.cpp:200-210`, `:337-345`; `$OPA/src/include/duckdb/decidb/formulation/solver_input.hpp:251,464,502,706`).
- **Two executors for one check.** Row checks run as relational operators on the direct path and inside the tree on the solver path (`$OPA/src/optimizer/decide/decide_optimizer.cpp:118`, `$OPA/src/include/duckdb/optimizer/decide/direct/direct_row_checks.hpp:26-32`). ANR installs them in the plan on both paths.
- **Transitional residue.**
  - `retained_*` side tables, commented "during the transition" (`$OPA/src/include/duckdb/planner/operator/decide/logical_decide.hpp:734-754`).
  - "P9 shadow" fields (`decide_tree.hpp:229-233`, `:308`, `:318`).
  - A fault-injection hook in tree construction (`decide_tree.cpp:1153-1157`) and a second in the columns (`decide_tree_columns.cpp:871-899`).
- **Dead code (certain).**
  - `DecideEvaluationFields`, `Id`, `Ids`, `Mask` (`physical_decide.cpp:893-949`).
  - The `silent` parameter, only ever `false` (`:788`, `:834`).
  - An unreachable first-row fallback (`:842-846`).
  - `ExecuteOnReadRows` and `DecideListRowMembers` (`$OPA/src/execution/operator/decide/decide_value_extraction.cpp:117-168`, `:910`).
- **Good.** The no-value rule is one class (`DecideAbsence`). A key's groups are numbered once, so the group cache and name resolver are gone. The 6787-line file is split.

## Q3. Base for step 5

Both branches create the helper at plan time and inline in stage 06. `InlineReducerValueLinks` and `EmitExtremumLink` already produce "helper per MAX group, SUM added upward", but by a global after-the-fact analysis (`PlaceHelpers`), not at the node.

OPA's history records one step-5 attempt that was built and then removed: commit a4263883ff says 535 lines, no stage 06 code removed, no model changed, and that it hid a nest from the D89 inlining rule. That is a commit message, not something I verified. It does show that composing at nodes beside stage 06 does not work; step 5 has to replace stage 06's helper logic, not sit next to it.

### OPA: what step 5 needs

- **Add.**
  - A per-slot symbolic value for decision-valued nodes. `ReducerValueMembers` is already that shape, and `ValueLinks` already walks `tree.eval_order` (`decide_model_emitter.cpp:2117-2243`).
  - A node-local rule: SUM and AVG pass members up; MIN and MAX allocate a variable per group plus envelope rows.
  - Run-time variable allocation in `VarIndexer`.
- **Change.**
  - Build G nodes from the written nest, not from `reducer_value_links` (`decide_tree.cpp:1960-2018`).
  - Make one representation the source of truth for a body.
  - Have roots read child forms per instance instead of scattering to rows.
- **Delete.**
  - `AddValueHelper`'s allocation.
  - `InlineReducerValueLinks` and `PlaceHelpers` (about 600 lines).
  - The `value_nodes` plumbing.
  - Helper keyed scopes and `HelperMapping`.
  - Whichever of the flat records or the algebra loses.
- **Survives.** Tree structure, descent, merge, key map, data programs and folds, frames, absence. Roughly 60-70% of the evaluator by my estimate; this is not measured.

### ANR: what step 5 needs

There is no node to write at. Step 5 first needs a run-time tree and nested per-node containers, because `PerGroupCache` is flat per key (`$ANR/src/include/duckdb/execution/operator/decide/physical_decide.hpp:38-57`). `EvaluateConstraints` would have to be decomposed per node. That is rebuilding OPA's steps 1-4.

- **Survives.** The row store (363 lines), frame code, extraction helpers, stage 06, and the verify harness as a parallel-run tool.
- **Replaced.** About 3000 lines of `physical_decide.cpp:2448-5360`.

### Shared blockers on either branch

- The D89 rule and implied-bound propagation need every reader of a helper, which a node does not see.
- `FormulateModel` must stay re-runnable against the widened DIAGNOSE box without reading data (`$OPA/src/include/duckdb/execution/operator/decide/physical_decide.hpp:327-337`). Node-time writing must therefore not bake in Big-M constants.

## Q4. Defects and risks

| # | Branch | Finding | Location | Confidence |
|---|---|---|---|---|
| 1 | OPA | A collation error in a frame's order key is attributed to the wrong row. `FrameOrder` passes positions `0..n-1` as rows, and `Evaluate` records `row_base + rows.rows[i]`. Wrong whenever the stored rows are a strict subset of the chunk. | `decide_descent.cpp:1361-1366` with `:577-586` | likely |
| 2 | OPA | `getenv` twice per chunk per thread in `Sink`; test hooks (`DECIDB_DESCENT_CHUNK`, `DECIDB_DUMP_STEPS`, `DECIDB_DESCENT_SPLIT`) on the hot path. | `physical_decide.cpp:316,325,366` | certain |
| 3 | OPA | `DECIDB_EVALUATOR_INJECT=g1` is read into a process-lifetime static that breaks reducer lookup in production code. | `decide_tree.cpp:1153-1157` | certain |
| 4 | Both | Unbounded error storage: one `ErrorData` per failing row, though only the least is thrown. | OPA `decide_error_ledger.hpp:74-83`, `decide_descent.cpp:581-586`; ANR `decide_row_store.cpp:75` | certain |
| 5 | OPA | After a vector fails, a new `ExpressionExecutor` is built per row (up to 2048 per failing chunk). ANR reuses one until a row fails. | `decide_descent.cpp:556-570`; ANR `decide_row_store.cpp:59-78` | certain |
| 6 | OPA | Carried columns are copied per node into unmanaged memory, with no sharing across clauses. A per-row node also stores `rows` and `parent_slots` per entry. | `decide_descent.cpp:389`, `:1401-1406`, `:1451-1462` | certain on mechanism; size unmeasured |
| 7 | ANR | One flat `Vector(num_rows + 1)` per distinct expression plus a copy of each kept input column, unmanaged. | `decide_row_store.cpp:112`, `:167-171` | certain |
| 8 | OPA | The merge is serial over every slot and set-E entry. Decision classes are matched by a string of type names plus sort key, a second identity mechanism beside the hash table. | `decide_descent.cpp:1772-1811`, `:2187-2192` | certain on shape; correctness of the equivalence unverified |
| 9 | OPA | `TaskExecutor` has no waiting destructor. An allocation failure between `ScheduleTask` and `WorkOnTasks` would leave tasks holding dangling stack references. | `decide_descent.cpp:2120-2176`, `$OPA/src/parallel/task_executor.cpp:15` | speculative |
| 10 | ANR | All expression evaluation is single-threaded in `Finalize`, and runs on rows a `when` drops. | `decide_row_store.cpp:192-201` | certain |

I found no data race in the per-thread descent or in `CopyMergedParts` by reading. Writes are disjoint per window and per part, and the maps merge serially.

## Overall recommendation

Keep OPA, because step 5 is decided and only OPA has nodes with identity and containers; ANR would have to grow them first. Do not start step 5 on OPA as it stands. Pay down P1-P3 first, or step 5 becomes a fourth representation beside three. If step 5 were not planned, ANR would be the simpler and more conventional operator to keep.

## Ranked proposals

**P1. One representation of a body.** Severity: major. Confidence: certain.
- Location: `decide_tree.hpp:30-50,123-162`; `decide_tree.cpp:732-1136,2359-2889`; `decide_model_emitter.cpp:28-310`.
- What's wrong: the algebra is derived from the flat records and then checked against them twice.
- Why it matters: step 5 needs exactly one operator tree per node; today it would be a third consumer of a mirror.
- Refactor: make the algebra the source and have stage 05 lower into it. Delete `DecideBody`'s term vectors, the `Algebra*` checkers and most of `Validate`. Replace `string operation` with an enum.
- Alternative: delete the algebra now (about 1000 lines, no behaviour change) and reintroduce a typed one with step 5.
- Case list and size: none added. Net removal either way.

**P2. Give error order its own rule.** Severity: major. Confidence: certain on the coupling; the rule is a spec call.
- Location: `decide_error_ledger.hpp`; `decide_tree.cpp:1277-1380`; `decide_tree.hpp:179-190,266-275`.
- What's wrong: ordering reproduces a deleted implementation through a per-kind case list.
- Why it matters: every new construct must invent a rank in an evaluator that no longer exists.
- Refactor: define one order from the written query (clause order, then row), keep only the minimum error per descent, and delete `DecideBlock` and the seven per-node rank fields.
- Alternative: keep the order but document it as the specification, without "flat".
- Case list and size: removes the list. Roughly 300 lines removed. Error-order tests change.

**P3. Emit instances, not rows.** Severity: major. Confidence: likely.
- Location: `decide_model_emitter.cpp:916-1008,1375-1479`.
- What's wrong: entry values are scattered to `num_rows`-dense vectors and regrouped by stage 06.
- Why it matters: this is the adapter step 5 must remove anyway.
- Refactor: emit CSR rows per instance (`EmittedRows` already exists in `solver_input.hpp`).
- Alternative: leave it until step 5 and do both together.
- Case list and size: none added. Size neutral now; removes the scatter and `vector<bool>(num_rows)` code later.

**P4. Remove residue.** Severity: minor. Confidence: certain.
- Dead code listed in Q2; the `value_nodes` cross-check; the duplicated sense and aggregate readers; `retained_*` comments; the env hooks (move to a debug setting read once).
- Case list and size: about 250 lines removed, none added.

**P5. Split `DecideNode` by kind.** Severity: minor. Confidence: likely.
- A base node plus Scope, Reducer, Frame and Function subclasses with `Cast<T>()`, as DuckDB's `Expression` does.
- Alternative: keep one struct and group the per-kind fields in nested structs.
- Case list and size: size neutral; removes fields that are invalid for a node's kind.

**P6. Rebuild a parallel run for step 5.** Severity: minor. Confidence: speculative.
- OPA deleted the in-engine oracle. Comparing `SolverModel` between stage-06 formulation and node formulation would give step 5 the harness ANR had (`$ANR/src/include/duckdb/execution/operator/decide/decide_evaluator.hpp`).
- Size: about 400 lines, temporary by design.

## Open design calls for you

1. Which branch to keep. My recommendation is OPA.
2. P1 direction: algebra as the source, or delete the algebra until step 5.
3. Whether error order may change from the flat evaluator's (P2).
4. D89 under step 5: decide inlining at the node (SUM always added upward, as the deck shows), or keep a global pass.
5. Whether helper variables are allocated at run time. This affects EXPLAIN, DIAGNOSE naming and the elastic re-formulation.
6. Whether row checks and data reducers on the solver path stay in the tree or return to relational operators as in ANR.

## Not determined by reading alone

- Whether the two branches produce the same models, and whether either passes its suite. Nothing was run.
- Relative speed and peak memory, including whether OPA's per-node copies exceed ANR's per-expression slots on real queries.
- Whether merged numbering always equals a single descent's: sort-key equality versus hash-table equality for class identity, and the `MIXED_ENCLOSING` case.
- Whether `GroupedAggregateHashTable` row addresses stay stable across resizes in this DuckDB version, which `DecideKeyMap` relies on (`decide_key_map.cpp:51`).
- Whether defect 1 is reachable; it needs a collation expression that throws.
- Thread safety under a real parallel sink; no sanitizer was run.
- Whether the header comment that only the objective reads `routed` is stale. Five read sites exist (`decide_model_emitter.cpp:341,1728,1740,1864`; `decide_tree_columns.cpp:384`).
- The "survives" percentages in Q3; they are estimates.

Out of lens: ANR's and OPA's user-visible error order and messages may differ in documented edge cases. That belongs to the requirements review.
