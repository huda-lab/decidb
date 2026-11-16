<!-- Full report of a review agent, saved 2026-11-16. Model output, not verified unless the main report (../handoff.md) marks a finding as re-checked. -->

# Integration cost: Sami-ANR (A) and Sami-OPA (B) into direct-solve-prototype

**Bottom line:** B costs about twice as much as A to integrate, and the difference is not driven by the 84 shared language commits. The difference comes from what B's 140 extra commits rewrote. They replaced the execution layer and the stage-05 term structures. Since the fork, mainline has also rewritten both:
- **Product form (`fd0994366d`):** every degree-2 term now travels as one product group (`DecideProductGroup`). The old `bilinear_terms`, `quadratic_groups` and `squared_terms` structures and `DecideTermKind` are gone.
- **One objective sense (`7fde155940`):** the canonicalizer writes every objective as a minimization and keeps the written direction in `written_sense`.
- **BLOAT clean-up:** about 960 lines deleted from `physical_decide.cpp`.
- **Fragment tags replaced by source spans (`1cd018decc`).**

Everything below used in-memory plumbing only. Nothing was built, tested, checked out or committed. Scratch output is in `/private/tmp/claude-501/-Users-hatim--claude-spares-spare-Ungos1/c596838a-2de5-42c6-8006-2a00850e535d/scratchpad/merge/`.

## 0. Most of the raw conflict is formatting

Mainline commit `da8b8941af` (FMT-1) ran clang-format over all DeciDB sources. I checked that it is exactly reproducible: clang-format 11.0.1 from the test venv, with the repository's `.clang-format`, reproduces FMT-1 byte for byte on `physical_decide.cpp`, `ilp_model_builder.cpp` and `decide_binder.cpp`.

I then re-ran every conflicted source file as a three-way merge with base, mainline and candidate all clang-formatted (`git merge-file --diff3`).

| `src/` conflicts | Raw files | Raw hunks | Raw lines (main + cand) | After format: files | After format: hunks | After format: lines |
|---|---|---|---|---|---|---|
| A (ANR) | 53 | 270 | 10,869 | 45 (8 become clean) | 183 | 3,884 |
| B (OPA) | 58 | 347 | 14,003 | 53 (5 become clean) | 271 | 7,269 |

- **Step 1 of any plan:** commit a clang-format of the candidate tip on the candidate side, then merge. This removes 64% of A's conflict lines and 48% of B's.
- **What is left after that:** B has 1.9 times A's real conflict, and 2,138 of B's 3,385 extra lines are in `physical_decide.cpp`.
- **Generated grammar files:** `gram.hpp` and `src_backend_parser_gram.cpp` conflict in both (about 40k lines). The grammar sources merge cleanly, so `make grammar-build` regenerates these.

## 1. What mainline changed since the fork (`src/`: 186 files, +22,309 / −18,279)

**Execution: `physical_decide.cpp` and `.hpp`**
- Ignoring formatting, mainline added 458 lines and removed 962.
- About 21 commits touch it. The behavioural ones:
  - `fd0994366d` (+204/−428): product groups.
  - `7fde155940`: one objective sense.
  - `b5167002f8`: a `per K:` clause with a `by` reducer gates each `by` group (around line 2271).
  - `a409a9503b`: a composed MIN/MAX under `per K:` has no instance when no kept row has a class.
  - `22e2a72fe1`: NaN or infinite coefficients are named as such.
  - `1cd018decc`: messages quote source spans.
  - `5b14e8697b` (BUG-45), `ae8ed8352d` and `b7743c0b5b` (number warnings), `bcc45b4e84` (BUG-54).
- BLOAT-24/25 deletions (`88c5f87385`, `b0a489da4e`, `b20600d087`): no behaviour change.

**Physical planning: `plan_decide.cpp`**
- Formatting plus the prepared-model and sense changes.
- Ignoring formatting, A has no conflict here and B has 2 hunks.

**`logical_decide.hpp` / `.cpp`**
- `decide_sense` is renamed `written_sense` (serialized id 203).
- `source_fragments` (id 215) is deleted in favour of `DecideSourceText source_text` (id 222).
- `objective_canonical_differs` is new (id 221).
- Determination checks gain `key_order`.

**Optimizer, non-direct**
- `decide_linear_form.cpp` is rewritten onto product groups.
- `decide_rewrite_minmax.cpp` changes for the sense.
- The ABS check moves: `decide_rewrite_abs.cpp:436` now pushes a scope check, so `install_row_checks` runs after `optimizer.abs` (`decide_optimizer.cpp:128,130`).
- The two-level objective is kept.
- `decide_degree.cpp` and `.hpp` are deleted (`c8c491d359`). `DecideDegreeOf` in `decide_product_form.hpp:78` and the new `decide_shape_check.cpp` replace them.

**Optimizer, direct**
- New rules: A2, N1, O1, R4, S2, S3, plus `direct_ranked_prefix`.
- `direct_plan_fragments` grows by about 830 lines. It gains the shared `DirectAggregateSink` (BLOAT-1), `DirectAddInstanceFlag` and a class key with no flag (`60566255a5`).
- `direct_bound_reader` grows by about 490 lines.
- `direct_ir` gains table facts (NOT NULL/UNIQUE), `model_shape` and `ReadKey`.
- R4-4 (`a0475b387f`) implements a keyed decision as a hash aggregate per key.
- Every `sense` parameter is gone from the rules and from `DecideModelShape`.

**Formulation (`src/decidb/formulation/`)**
- `ilp_model_builder` and `solver_input` move to product groups and lose `sense` and `maximize`.
- The other `linearization_*` changes are mostly formatting plus sense.

**Serialization**
- See section 3c.

## 2. Conflicted source files and their classification

Sizes below are hunks / lines (mainline side + candidate side) after format normalisation; "same" means B's size equals A's.

Classes:
- **F**: formatting only, clean once formatted.
- **M**: mechanical.
- **St**: structural, meaning one side moved, deleted or rewrote what the other edited.
- **Se**: semantic, meaning both sides changed what the same mechanism means.

| File | A | B | Class and reason |
|---|---|---|---|
| execution/operator/decide/physical_decide.cpp | 38 / 800 | 59 / 2,938 | A: St+Se. Mainline rewrote the quadratic evaluation into `EvaluateProductGroups` and deleted arms; ANR edited the old internals (frame row windows, `ReadVariable`). B: **St**. 19 hunks hold 1,802 mainline lines in regions OPA deleted (OPA's file is 2,523 lines against mainline's 4,350). Mainline's behavioural commits must be re-done in the tree files. |
| decidb/formulation/ilp_model_builder.cpp | 4 / 47 | same | Se, small (product groups vs candidate fields) |
| decidb/formulation/linearization_minmax.cpp | 2 / 193 | same | Se: sense rewrite vs ANR's removal of the two-level form |
| planner/decide/decide_term_split.cpp | 2 / 260 | 6 / 591 | **Se**: mainline's product form (degree/opaque/factors) vs `DecideTermKind::FRAME`; OPA adds bilinear function groups |
| planner/expression_binder/decide/decide_binder.cpp | 19 / 367 | same | Se, mixed: shape check / degree deletion vs nested reducers and frames |
| optimizer/decide/decide_linear_form.cpp | 10 / 193 | 17 / 368 | **Se**: product groups vs qualifier_reducer, child and frames |
| planner/decide/decide_canonicalizer.cpp | 14 / 379 | same | **Se**: determination removed and variants added (D70) vs `judge_column_refs` removal and the sense flip |
| planner/decide/decide_source_provenance.cpp / .hpp | 12 / 191 and 3 / 43 | same | Se: source spans vs fragments; two-level renderers removed by ANR |
| optimizer/decide/direct/direct_ir.cpp | 6 / 137 | same | Se: mainline table facts and `model_shape` vs frames, signatures and deleted `ReadDetermination` |
| optimizer/decide/direct/direct_row_checks.cpp | 1 / 134 | same | St: candidates deleted `DeterminationCheck`, mainline edited it (BUG-54). B also replaces Install with Retain (§3a). |
| optimizer/decide/decide_rewrite_minmax.cpp | 4 / 103 | 5 / 113 | Se: sense vs two-level removal |
| planner/expression_binder/decide/decide_objective_binder.cpp | 4 / 106 | same | St: ANR removed `BindTwoLevelObjective`, mainline edited it |
| decidb/formulation/linearization_bilinear_abs.cpp | 5 / 69 | same | Se, small (sense) |
| common/decide_value_messages.hpp | 2 / 69 | same | St: `DecideDeterminesText` deleted vs `InKeyOrder` added |
| decidb/formulation/linearization_reducer_value.cpp | 4 / 63 | same | Se: sense flip in helper links |
| optimizer/decide/decide_rewrite_scoped.cpp | 5 / 64 | 6 / 72 | Se: extremum push via `decide_sense` (§3) |
| optimizer/decide/decide_solver_gate.cpp | 3 / 55 | 9 / 132 | Se: model shape and sense. OPA derives the model class from the tree. |
| planner/decide/decide_tree_terms.hpp (renamed from decide_prepared_model.hpp) | 1 / 32 | 4 / 145 | **Se**: `product_groups` vs OPA's extended `BilinearTerm` / `QuadraticGroup` / `squared_terms` |
| storage/serialization/nodes.json | 1 / 86 | same | M, but needs renumbering and a decision (§3c) |
| storage/serialization/logical_operator.json and serialize_logical_operator.cpp | 1 / 26 and 2 / 12 | same | M, with an id collision (§3c) |
| storage/serialization/serialize_nodes.cpp | 2 / 45 | same | M: regenerate |
| r1_continuous_knapsack_rule.cpp | 1 / 48 | same | St: mainline moved R1's sink into `DirectAggregateSink`; the candidates edited it for vector arguments |
| direct_plan_fragments.cpp / .hpp | 1 / 6 and 1 / 8 | same | Se: `DirectAggregateOf(…, vector arguments)` vs mainline callers (§3) |
| direct/direct_rule.hpp | 2 / 11 | same | St: the DETERMINES row-check kind deleted vs edited |
| direct_source_reader.cpp | 1 / 7 | same | M |
| planner/binder/query_node/plan_select_node.cpp | 3 / 35 | same | M with some sense edits |
| planner/operator/decide/logical_decide.hpp | 2 / 30 | 3 / 37 | M (field lists) plus determination `key_order` |
| planner/operator/decide/logical_decide.cpp | 1 / 54 | same | M |
| optimizer/decide/decide_rewrite_rhs_bounds.cpp | 4 / 26 | 5 / 32 | M/Se: `argument` becomes `arguments` |
| optimizer/decide/decide_rewrite_abs.cpp | 2 / 20 | 8 / 168 | A: M. B: Se (ABS pass order and helpers) |
| optimizer/decide/decide_rewrite_define.cpp | 3 / 10 | n/a | A: M |
| decide_tree_definition.cpp (renamed from decide_rewrite_define.cpp) | n/a | 10 / 81 | B: St (`DefineNonlinearReducerBodies` replaced by `PrepareDefinitionFunctions`) |
| include/duckdb/planner/decide/decide_term_split.hpp | 2 / 35 | 5 / 93 | Se, as for the `.cpp` |
| Small M files (each ≤ 26 lines, same in both) | | | decide_binder.hpp 3 / 15; enums/decide.hpp 1 / 26; ilp_linearization.hpp 1 / 11; physical_decide.hpp 1 / 8 in A, 2 / 13 in B; decide_canonicalizer.hpp; decide_constraint_walk.hpp; constraints and declarations binders 2 / 18 and 2 / 19; linearization_bigm 1 / 3; planner/decide/CMakeLists 1 / 3 |
| F (clean once formatted) | | | decide_diagnostic.cpp, linearization_not_equal.cpp, deterministic_naive.cpp, ilp_model.hpp, decide_objective_binder.hpp. Also plan_decide.cpp, solver_input.hpp and decide_rewrite_norm_in.cpp, which are F in A but have 2 / 8, 1 / 3 and 2 / 11 left in B. |
| B only | | | decide_rewrite_bilinear.cpp 9 / 152 (Se: OPA's bilinear function groups vs product form); decide_optimizer.cpp 6 / 22 and decide_optimizer.hpp 1 / 13 (**Se**: pass order and side plans, §3a); decide_rewrite_notequal_avg.cpp 3 / 19 (M); decide_solver_gate.hpp 2 / 10 (M) |
| modify/delete | none | decide_degree.cpp | B adds a `DEFINITION_FUNCTION_TAG` rule (`decide_degree.cpp:53,136`); mainline deleted the file. The rule must move into `DecideDegreeOf`. |

**Docs and tests (not in the counts above).** The two candidates are practically identical here:
- Docs: 27 conflicted `context/docs` files in A and 30 in B.
- Tests: 23 conflicted test files in each, plus 2 modify/delete. `test_scope_determination.py` and `test_scope_check_plan.py` are deleted by both candidates and modified by mainline.
- Goldens: `baseline.*.dump` has 7 hunks in each. B's golden dumps differ from A's by one line, so the tree reproduces the flat evaluator's models.

## 3. Semantic collisions git does not flag

I took identifiers declared in the base's DECIDE headers that one side removed, and searched for them in lines the other side added.
- **A:** 9 hidden references, in `decide_rewrite_frames.cpp` (2), `direct_bound_tables.cpp` (1) and `test_decidb_helper_inlining.cpp` (6).
- **B:** about 160 references in 11 files that merge without a conflict marker: `decide_model_emitter.cpp` 63, `decide_tree.cpp` 38, `decide_tree_compile.cpp` 21, `decide_value_extraction.{cpp,hpp}` 11, `decide_global_sink_state.hpp` 5, `decide_singleton_row_folding.cpp` 2, `decide_rewrite_frames.cpp` 2, and others. Nearly all are `bilinear_terms`, `quadratic_groups`, `squared_terms`, `has_quadratic`, `DecideTermKind`, `SplitLinear` and `DecideExpressionDegree`. B's tree back end is built on the degree-2 representation mainline replaced.
- **Reverse direction:** mainline's new code barely depends on anything the candidates removed. The one exception is (d) below.

### (a) Side plans

**What B changed**
- B replaced `DirectInstallRowChecks` (mainline `direct_row_checks.cpp:234`) with `DirectRetainRowChecks` (B `direct_row_checks.cpp:98`).
- On the solver path, the source is no longer gated by a row-check CTE and no longer joined to the `DirectAddBoundTables` side tables.
- Instead, the checks go to `LogicalDecide::retained_row_checks`, the bound aggregates go to `retained_bound_aggregates`, and the tree evaluates both (`decide_tree.cpp:208,222,374`).
- `RepointSourceColumns` and the `ColumnBindingReplacer` use are gone.
- The direct path is unchanged. `DirectAddRowChecks` and `DirectAddBoundTables` stay, and every mainline rule (R4, S2, S3, A2, N1, O1) keeps working with them.
- B's only other direct/ change is a `held` parameter on `DirectTightestBound` and `DirectBoundDisagreement` (`direct_plan_fragments.cpp:94-104`).

**Collisions that follow**
1. **The ABS check is lost on the solver path in B.** Mainline moved `install_row_checks` to run after `optimizer.abs` (`decide_optimizer.cpp:128,130`) because `AddAbsValueCheck` pushes a scope check (`decide_rewrite_abs.cpp:436`). B runs `retain_row_checks` at line 123 and `abs` at line 154. That check would sit in `scope_checks` with nothing consuming it. The text conflict shows only the moved line, not this effect.
   - A's merge auto-resolves to mainline's order (install after abs). That order also runs after ANR's new `frame_ranges` and `nested_lift` passes; the order needs a test.
2. **Mainline's side-table fixes no longer reach B's solver path.** These need parity tests against B's tree evaluation of retained aggregates:
   - `60566255a5`: a NULL class key; each key element compared alike.
   - BUG-45.
   - The `DecideGroupBound` work in `direct_plan_fragments`.
   - The tests are `test_bound_side_tables.py`, which both candidates rewrote (212 lines), and `test_direct_class_key.py`, which is new in mainline.
   - Mainline's stated design, "one implementation makes them on both paths" (the comment at `direct_row_checks.hpp:35`), becomes two implementations.
3. **Two fixes for the same "group with no instance" problem.** Both landed on 2026-11-13:
   - Mainline `b5167002f8` (`physical_decide.cpp` around line 2271, and `DirectAddInstanceFlag` at `direct_plan_fragments.hpp:535`).
   - Mainline `a409a9503b`.
   - OPA `e153b7daf3` (`DecideGroupBound::held`).
   - Mainline's new callers of `DirectTightestBound` (`direct_bound_reader.cpp`, `a2`) use the default `reads = {}`, so they get none of OPA's `held` semantics.
4. **R4 keyed / hash aggregate per key and sign handling.** R4 does not call row checks, determination or side plans; it reads `ir.keyed_scopes`. It should keep firing after merge if `direct_ir.cpp` keeps mainline's `ReadKey` and table facts and adds the candidates' declines (`scope.signature`, `objective_signature`, frames). Mainline's sign handling (`ReadModelShape` / `ScaleSignAtPlanTime` in `direct_ir.cpp`) sits inside conflict hunk 5 and is "keep mainline". Neither candidate touches it.

### (b) Evaluator bypass, and structures the candidates reshaped

**Bypass.** In both candidates `ChooseDecideAnswer` runs before any evaluator work and replaces the node, so neither the flat evaluator nor the tree runs when a rule fires. B's `PrepareDefinitionFunctions` and tree construction run only in `RewriteForSolver` and `plan_decide`. The direct front end declines on frames, on a reducer's own `per` with a signature, and on an objective signature (B `direct_ir.cpp:69,152,485`).

**Reshaped structures.** Ranked by impact:
1. **Product form vs the old degree-2 structures (Se, both candidates; B much more).**
   - Mainline: `DecideProductGroup` (`decide_prepared_model.hpp:70`) and `DecideTermSplitter` without `DecideTermKind`.
   - A adds `FRAME` (`decide_term_split.hpp:53`) and fields such as `qualifier_reducer`, `child` and `avg_group_key` to `BilinearTerm`, `QuadraticGroup` and `DecideTerm`.
   - B deletes `DecidePreparedModel` entirely and builds its emitter on the old structures, for example `decide_model_emitter.cpp:25,38` and `decide_solver_gate.cpp:154`. The degree-2 part of the tree emitter needs re-design onto product groups, not a rename.
2. **Objective sense trap (both candidates).**
   - The candidates choose the extremum push from `decide.decide_sense`: A and B `decide_rewrite_frames.cpp:257`, B `decide_rewrite_scoped.cpp:640`, B `decide_tree.cpp:970,2527-2549`.
   - Mainline renamed the field to `written_sense` (`logical_decide.hpp:351`) and already folded the flip into the term signs.
   - It will not compile, and the obvious fix (rename the field) silently flips twice. The fix is to always push DOWN, or read the term sign.
3. **`decide_degree` deleted on mainline.**
   - A's `decide_rewrite_frames.cpp:114` and the merged hunks still call `DecideExpressionDegree`.
   - B also changed the degree's meaning: a definition-function tag counts as a decision.
   - Both must move to `DecideDegreeOf` (`decide_product_form.hpp:78`).
4. **Source fragments vs source spans.**
   - B's `DirectRetainRowChecks` writes `decide.source_fragments` (`direct_row_checks.cpp:123-131`), a field mainline deleted.
   - The candidates use fragment-tag APIs that mainline deleted: A 3 times, B 15 times.
5. **`DirectAggregateOf(…, vector<arguments>, …)` (both candidates).** The candidates changed the signature for collated MIN/MAX. Mainline's new shared `DirectAggregateSink::Add` (`direct_plan_fragments.cpp:888`) still passes a single argument. This is a compile break that stays hidden after the text merge.
6. **`variable_scopes` invariant (B).** Mainline removed the bounds fallback (`decide_optimizer.cpp:249`: the scopes run parallel to the decisions). B's value helpers rely on the fallback (`decide_optimizer.cpp:218,251`).

### (c) Serialization

The `LogicalDecide` changes are identical in A and B; B's retained fields are not serialized.
- **Property id collision in `LogicalDecide`:**
  - Mainline uses 221 `objective_canonical_differs` and 222 `source_text`.
  - The candidates use 221 `signatures`, 222 `objective_signature` and 223 `frames`.
  - Renumber the candidates' to 223–225 and regenerate.
- **`DecideScopeCheck`:**
  - The candidates mark field 110 `key` deleted because determination checks are gone.
  - Mainline adds 111 `key_order` for BUG-54, which is meaningful only for determination.
  - If the candidates' language change (D70 variants) is accepted, `key_order` and `InKeyOrder` go too.
- **Mainline-only changes:** the rename of 203 to `written_sense` and the deletion of 215 `source_fragments` are on mainline alone; the candidates do not touch them.
- **Validate with** `DECIDB_VERIFY_SERIALIZER=1`.

## 4. Tests

- **Test conflicts are a tie.** Both candidates conflict on the same 23 test files and the same 2 modify/delete files. A and B both-modified 47 and 48 files with mainline. B's only extra overlap is `test_like_term_collection.py`.
- **Mainline tests whose expected behaviour both candidates change.** These are determination errors that become variants (D70), plus golden entry 101:
  - `test_scope_determination.py` (17 asserts) and `test_scope_check_plan.py` (6), both deleted by the candidates.
  - `test_message_source_spans.py:59`: new on mainline, merges cleanly, and fails silently.
  - `test_qualified_reducer.py` (6), `test_scope_reducer_inner.py` (8), `test_collated_keys.py` (2), `test_scope_parser.test` (2) and `test_direct_s1_uniform_matroid.py` (2), all conflicted.
  - `test_scope_combinations.py`: its oracle `_scope_oracle.py` raises determination errors.
  - Possibly `test_per_class_null_key_by_group.py`, which is new on mainline.
  - **Owner decision:** whether D70 variants replace the determination error. Every item above follows from that.
- **B-only test cost:** B ships 259 golden-master snapshots in `test/decide/tests/expected/` (112 `model-*`, 147 `run-*`), read by `_expected.py`, `test_tree_emitter.py`, `test_error_precedence.py` and `_old_spellings.py`.
  - They were recorded from the flat evaluator, which B deletes.
  - 99 contain `sense: maximize` model dumps, which mainline's one-sense change makes stale. Product groups and span messages will invalidate more.
  - Re-recording them against the merged build turns them into self-referencing snapshots.
- **New mainline end-to-end tests that need a run on B:** `test_formulation_arms.py` and `test_optimizer_arms.py` target `physical_decide` arms that B moved, and `test_product_normal_form.py`. They merge cleanly but their purpose shifts under B.

## 5. Merge direction and the A–B difference

- **Do one three-way merge, not a rebase.** The conflict set is the same in either merge direction. I recommend merging the formatted candidate into an integration branch cut from mainline, so first-parent history stays mainline's.
  - Rebasing A replays 68 source commits: 36 of them touch `physical_decide.cpp`, 16 the canonicalizer and 15 the linear form. Each crosses FMT-1 and the product-form change again.
  - Rebasing B replays 182 source commits, including building and then deleting the flat evaluator and the evaluator switch (`b1f287ac65`, `19784f71b6`, `f12d9f9180`). That is pure waste.
  - Rebasing mainline (404 commits) is out of the question.
- **Effort estimate.** These are focused person-days by someone who knows both sides; they are my judgement, not measured.
  - **A: about 6–10 days.**
    - About 1 day mechanical: format pre-step, grammar regeneration, serialization renumbering, doc and test text conflicts.
    - 4–7 days semantic: port FRAME and `qualifier_reducer` onto product form (2–3); determination→variant semantics plus rewriting about 10 mainline test files (1–2); sense trap (0.5–1); spans vs fragments (0.5–1); ABS order, `DirectAggregateOf` and `decide_degree` (about 1).
    - 1–2 days validation and fixes across the full suite, goldens, the serializer run, HiGHS and the direct-solve-off matrix.
  - **B: about 14–22 days.**
    - Everything in A, with larger instances in the tree code.
    - Re-implement mainline's roughly 8 behavioural execution commits in the tree, descent and emitter, because B deleted the code they edit (3–5 days).
    - Re-design the tree emitter's degree-2 lowering onto product groups (3–6 days).
    - Side-plan parity: the ABS check, side tables and instance flags (1–2 days).
    - Re-record and review the 259 snapshots (1–2 days).
- **Is it dominated by the shared 84 commits?** No. The shared language work is roughly 40–50% of B's cost.
- **Should integration cost influence the choice?** Yes, but only for timing. If B's tree evaluator is the intended end state, choosing A now does not avoid B's cost; the tree would still have to be ported onto product-form mainline later. B also has an architectural consequence beyond cost: on the solver path it replaces mainline's single shared mechanism for checks and side tables with a second implementation.

## What I could not determine

- Exact compile breaks, and runtime differences in the merged trees. Building and testing were out of scope.
- Whether B's tree already reproduces mainline's per-`by`-group instance gating, the NULL class-key side tables, and BUG-45.
- Whether R4, S2 and S3 results change under variant semantics. I expect them to decline, since rules decline variant-bearing scopes and R4 rejects `per K:` budgets, but this needs a test run.
- How many of the 259 B snapshots change beyond the 99 maximize dumps.
- How intended each candidate's language semantics are. I treated `context/docs` as unverified and did not use it for any conclusion.
