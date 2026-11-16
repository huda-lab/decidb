<!-- Full report of a review agent, saved 2026-11-16. Model output, not verified unless the main report (../handoff.md) marks a finding as re-checked. -->

# Dead / vestigial / duplicated code audit: branch Sami-ANR (tip 300e387bb1)

Read-only. I edited nothing, built nothing, ran no suite or benchmark, and did not touch `build/`. Scratch scripts are in the scratchpad dir only. Worktree used: `/Users/hatim/Code/decidb/.claude/worktrees/sami-anr`.

Method:
- I stripped comments and strings from every file under `src/`.
- I counted whole-word occurrences of every identifier defined in the DECIDE scope, across all of `src/` and, separately, `test/`, `benchmark/` and `scripts/`.
- I read every candidate's lines by hand.
- "Fork" below means `cec0e42f99`. Where I say "pre-existing" the identifier already existed there.

Line counts are estimates of lines deletable. Paths are relative to the worktree root.

## 1. Defined but never called or referenced

| file:line | what | evidence | est. lines |
|---|---|---|---|
| `src/include/duckdb/decidb/formulation/solver_input.hpp:51-215` (`CoefficientColumn`) | The whole `Kind::SparseMasked` storage mode: `sparse_indices`, `sparse_value`, `MakeSparseMasked`, `IsSparseMasked`, and the `Get`, `EnsureDense`, `AllIntegral` and `AssignScalar` branches. The comment says it is "built for the NE per-row indicator path". | `MakeSparseMasked` occurs once in all of `src/` and `test/` (its definition). `IsSparseMasked` and `sparse_indices` are never referenced outside this struct. `linearization_not_equal.cpp` never builds one. Pre-existing. | ~35 |
| same struct | Unused members: `FromVector` (l.82), `AssignDense` (l.163), `MaskRow` (l.146), `ScaleRow` (l.147), `PushBack` (l.182), `Resize` (l.174). | Each occurs once. No `.Resize(` or `.PushBack(` on a coefficient column anywhere in `src/execution`, `src/decidb` or `src/optimizer`. Pre-existing. | ~40 |
| `src/optimizer/decide/decide_rewrite_bilinear.cpp:55-64` | `static GetSingleDecideVarIdx` | Occurs once in all of `src/` and `test/`. Pre-existing. | 10 |
| `src/include/duckdb/decidb/formulation/ilp_linearization.hpp:95-110` | `LinearizeMinMaxIndicators` declaration and its comment | There is no definition and no caller. The only other hits are in comments. Pre-existing. | 16 |
| `src/optimizer/decide/direct/r1_continuous_knapsack_rule.cpp:956-962` | `ReadOnBudgetRows` | Occurs once in all of `src/` and `test/`. Pre-existing. | 8 |
| `src/optimizer/decide/direct/direct_plan_fragments.cpp:282-302` and `src/include/duckdb/optimizer/decide/direct/direct_plan_fragments.hpp:229-235` | `DirectCheckRowBounds` | Only the declaration and the definition exist; no call. Pre-existing. | 28 |
| `src/execution/operator/decide/physical_decide.cpp:1804,1955` (`DecideGlobalSinkState::canonicalizer`) | A `DecideCanonicalizer` is built per query and never used. Its only constructor argument beyond the state's own members is `op.keys`, which the branch added. | `canonicalizer` appears only at those two lines. Nothing reads it. The sibling `ClientContext &context` member (l.1948) is then used only to initialise it. | 4 |
| `physical_decide.hpp:95`, `physical_decide.cpp:1696` (`decide_variable_map`) | Written in the constructor loop, never read. | `decide_variable_map` has exactly these two hits. Pre-existing. | 7 |
| `src/optimizer/decide/direct/direct_rule.hpp:50` and `direct_ir.cpp:133` (`DirectIRDecision::keyed_scope`) | Written, read only by `test/common/test_decidb_direct_ir.cpp:301`. | No reader in `src/`. Pre-existing. | 4 |
| `direct_rule.hpp:58` and `direct_ir.cpp:157-166` (`DirectIRKeyedScope::key_slots`) | Written, read only by `test_decidb_direct_ir.cpp:303` (`!...empty()`). | No reader in `src/`. Pre-existing. | ~8 |
| `src/include/duckdb/decidb/formulation/solver_input.hpp:443`, `linearization_bilinear_abs.cpp:256,293` (`range_unbounded`) | Written; read only inside `D_ASSERT` at `linearization_bilinear_abs.cpp:373`. | Pre-existing. | 4 |
| `physical_decide.cpp:142-150`, `decide_row_evaluation.hpp:26-31`, `:3078`, `:3119` (`InitializeDecideScan`, `DecideInputScans`, thread_local counter) | A counting wrapper around `data.InitializeScan`. | Its single caller is `decide_row_store.cpp:188`. The counter is read only to emit the profile counter `execution.input_scans`, which only `test/decide/tests/test_one_pass.py` reads, asserting `== [1]`. It is test and profile instrumentation, not behaviour. Branch-new (commit 047abf9489). | ~20 |
| `physical_decide.cpp:3913` | Profile counter `constraint.ordinal` | No test, benchmark or doc reads it. Debug output only. | 1 |
| `physical_decide.cpp:1790-1793`, `:2436-2442`, `:3411-3418` | Empty banner comments. "Evaluate Constraints" sits above the scope-checks code, but `EvaluateConstraints` is at l.3903. "PHASE 1.5" and "PHASE 1.4" headers have nothing under them. | Read directly. | ~15 |
| `src/include/duckdb/decidb/formulation/solver_input.hpp:790` (`ClassSet::Kind::WHOLE_CLAUSE`) | Produced at `physical_decide.cpp:1290`; never compared. | Only `Kind::KEY_VALUES` is tested, in `src/decidb/diagnostics/decide_diagnostic_render.cpp:654`. Not removable without checking that render path; flagged only. | n/a |

Also confirmed alive: every other `static` function in `physical_decide.cpp`, `physical_decide_frames.cpp`, `decide_row_store.cpp` and `decide_frame_map.hpp` has at least one caller. No enumerator has an occurrence count of 1.

## 2. Fields written but never read, or holding another field's fact

Confirmed write-only fields are in table 1: `canonicalizer`, `decide_variable_map`, `keyed_scope`, `key_slots`, and `range_unbounded` (assert only). Duplicate-fact fields are in table 4.

## 3. Constant parameters, unreachable branches, and the `DECIDB_EVALUATOR` scaffolding

| file:line | what | evidence | est. lines |
|---|---|---|---|
| `physical_decide.cpp:6053-6058,6478-6480` | `FormulateElasticModel(..., bool keep_singleton_rows, ...)` | The only caller passes `/*keep_singleton_rows=*/true`. The field `SolverInput::keep_singleton_rows` stays; only the parameter goes. | 3 |
| `physical_decide.cpp:168-226` (`ExecuteOnReadRows`, two overloads) | The pre-store chunk executor | Its single caller is l.4442 (`ExecuteOnReadRows(end_executor, end_input, 0, &members, ends)`). So `row_offset` is always 0 and `rows` is never null, which makes the `if (!rows) executor.Execute(...)` branch unreachable. The per-expression-reads overload is called only by the other overload. | ~25 (collapse 59 lines to ~35) |
| `physical_decide.cpp:296-300` (`ExtractDoubleColumn`) | `if (count == 0) return;` | Every caller passes `windows.Count()`, and `DecideRowWindows::Next()` returns false when the count is 0. Unreachable. | 3 |
| `physical_decide.cpp:3118,3169,3645-3659` and `physical_decide.hpp:353,396-399` (`bool silent`) | Parameter of `Evaluate` and `ReportSkippedInstances`. | It is `true` only for the second run under `verify`. | 4 |

### The `DECIDB_EVALUATOR` scaffolding (branch-new, commit 2188ae3f36)

Reachable only through `DECIDB_EVALUATOR=verify`, `tree` or `tree-noside`:

| piece | file:line | lines |
|---|---|---|
| Enum, env parsing, comparator, ulp compare, inject switch (`DECIDB_EVALUATOR_INJECT`) | `src/execution/operator/decide/decide_evaluator.cpp` (whole file) | 113 |
| Enum, `DecideEvaluationRecord`, compare declaration | `src/include/duckdb/execution/operator/decide/decide_evaluator.hpp` (whole file) | 57 |
| `DecideEvaluationFields` plus its `SwapWith` | `physical_decide.cpp:3176-3213` | 38 |
| Record helpers `Id`, `Ids`, `Mask`, `ValuesAt`, `Everywhere`, `NameByValue`, `RecordClause`, `RecordComposedTerm` | `:3215-3298` | 84 |
| `RecordEvaluation` | `:3300-3365` | 66 |
| `VerifyEvaluation` | `:3367-3409` | 43 |
| `switch (SelectedDecideEvaluator())` in `Finalize` with the `NotImplementedException` default | `:3059-3070` | 12 |
| `#include`, forward declaration, `silent` parameter, the two method declarations and their comments | `physical_decide.cpp:2`; `physical_decide.hpp:22,353,394-408` | ~20 |
| CMake entry | `src/execution/operator/decide/CMakeLists.txt` | 1 |
| **Src total** | | **~436** |

- `TREE` and `TREE_NOSIDE` are only parsed, then hit the "not built yet" throw (`physical_decide.cpp:3069`). `TREE_NOSIDE` appears only in the parser (`decide_evaluator.cpp:26`) and the enum.
- Under `verify` the second run is flat again. It compares flat against itself.
- Test-side:
  - `test/decide/tests/test_evaluator_verify.py` (93 lines) exercises only the harness.
  - Two pins to `flat`: `test/decide/golden/capture.sh:80` and `test/decide/tests/test_scope_reducer_by.py:261`.
  - The suite-log tooling's comments and README text name `DECIDB_EVALUATOR=tree` as its purpose: `test/decide/suite_log.py` (292), `capture_suite_log.sh` (105), `tests/test_suite_log_compare.py` (460), README l.345. That tooling is generic capture and compare (~860 lines). Whether to keep it is a separate decision.
- How cleanly it comes out: very. It is purely additive.
  - Nothing outside these files references it.
  - Removal is: delete the two evaluator files, delete the l.3176-3409 block, replace the `switch` with a plain `Evaluate(...)` call (`Evaluate` itself can stay as a method), drop `silent`, drop the forward declaration and the two method declarations, and delete the CMake token and the test file.
  - `ResolveGroupNames` (called in `RecordEvaluation`) is also used in `Finalize`, so it stays.

## 4. The same fact in two representations

| pair | written / read | removable? | est. lines |
|---|---|---|---|
| `TermFilterState::has_filter` vs `!mask.empty()` | Written at `physical_decide.cpp:1254,1438,4711,4801,5315` (7 `= true` sites). Read at 64 sites. | Yes, I think. `mask` is assigned exactly when `has_filter` is set, and `Finalize` returns early for 0 rows. The empty-mask case at `num_rows == 0` is unverified. | ~10, touches ~64 sites |
| `ComposedMinMaxTerm::kind` (`SUM_KIND`/`MINMAX_KIND`), `agg_name` ("sum"/"avg"/"min"/"max"), and `ComposedMinMaxTermData::is_minmax` | `kind` set at `decide_rewrite_minmax.cpp:218`. `is_minmax` copied from `kind` at `physical_decide.cpp:5072`. Read at `linearization_minmax.cpp:836,847,921,1008`. `SUM_KIND` is never compared. | One of the three could go: `is_minmax` is `agg_name` ∈ {min, max}. | ~6 |
| `PhysicalDecide::decide_constraints` / `decide_objective` (Expression trees) vs `prepared` (flattened terms) | Trees are read only by `ParamsToString` (EXPLAIN, `physical_decide.cpp:1757-1778`) and `plan_decide.cpp:44-50,217`. All evaluation reads `prepared`. | No. EXPLAIN needs the tree. Noted only. | 0 |
| `DecideGlobalSinkState::singleton_row_boxes` / `user_singleton_row_bounds` (`physical_decide.cpp:1969-1970`) vs the same two on `PhysicalDecide` | Copied at l.1809-1810, read via `gstate`. `op` outlives `gstate`, so references would do. | Yes. | ~6 |
| `DecideRowStore::keep[]` vs `columns[i] != nullptr` | `keep` is written in `KeepColumnsOf` and read in `Build`. After `Build`, `columns` null-ness holds the same fact. | Partly. `keep` is the pre-pass intent. | ~3 |
| Expression-identity maps: `ChunkExprCache` (unlowered to lowered), `DecideRowStore::slot_at` (address) and `slot_hash` (hash), `DecideRowWindows::resolved` (address to slot) | Four lookups for one question, "which slot holds this expression". | Possibly `resolved` and `slot_at`, as caches over `slot_hash`. Unconfirmed. | ~10 |
| `DecideRowStore::RequireKept` vs the loop in `SliceColumns` (`decide_row_store.cpp:166-172, 258-265`) | Same "column read but not kept" check, same message. | Yes. | ~8 |
| Profile spans (`DecideProfileScope`) vs `DECIDB_BENCH` timers (`physical_decide.cpp:3020-3029,6101-6103,6557-6566`; `decide_optimizer.cpp:84-110`) | Two timing systems report model-build and solve time. | Possibly. Benchmarks read `DECIDB_BENCH`. | ~30 |
| `EvaluatedConstraint::row_group_ids` vs `group_offsets` / `group_row_ids` (CSR, `solver_input.hpp:382`) | The CSR is derived lazily at `ilp_model_builder.cpp:229` | No. A derived index. | 0 |
| `DecideFrameMap`: `anchors`, `timeline`, `peer`, `selected_peer`, `absent` (per-row vectors, INVALID elsewhere) and `peer_rows` / `peer_begin` (inverse CSR); `window` vs `window_anchor` | Inverse indexes. | Not clearly. Noted only. | 0 |

## 5. `DECIDB_*` switches and settings read in this code

| name | read at | test/doc use |
|---|---|---|
| `DECIDB_EVALUATOR` | `decide_evaluator.cpp:14` (static, read once) | `test_evaluator_verify.py`, `test_scope_reducer_by.py:261`, `golden/capture.sh`, `capture_suite_log.sh`, test README, `context/docs/pipeline/08_execution/done.md` |
| `DECIDB_EVALUATOR_INJECT` | `decide_evaluator.cpp:54` | `test_evaluator_verify.py` only. No doc. |
| `DECIDB_BENCH` | `physical_decide.cpp:3020`, `decide_optimizer.cpp:84` | benchmarks, `test_direct_user_facing.py`, `suite_log.py`, Makefile, several docs |
| `DECIDB_STATUS_MARKERS` | `physical_decide.cpp:6375` | `test/decide/decidb_cli.py`, `test_suite_log_compare.py`, test README. No doc. |
| `DECIDB_PROFILE` | `src/include/duckdb/common/decide_profile.hpp:34` (`getenv` on every `DecideProfileScope` construction, so many times per query) | tests, benchmarks, docs |
| `DECIDB_FORCE_SOLVER` | message text only at `decide_answer_choice.cpp:61`; read in `src/decidb/solver/ilp_solver.cpp:296` | many tests and docs |
| `DECIDB_NATIVE_CONSTRUCTS` | outside scope (`solver_registry.cpp:56,70`) | many tests and docs |
| `DECIDB_DUMP_MODEL`, `DECIDB_TIME_LIMIT` | outside scope (solver) | tests and docs |
| `DECIDB_PROFILE_SOLVER_LOG` | outside scope (`gurobi_solver.cpp:162`, `deterministic_naive.cpp:421`) | no test, no doc |
| `DECIDB_TEST_DIRECT_SOLVE`, `DECIDB_VERIFY_SERIALIZER` | not read by any `src/` file. Harness only (`decidb_cli.py`, `capture_suite_log.sh`, `run_direct_matrix.sh`). | tests and docs |

Settings read from DECIDE code, all registered in `src/decidb/diagnostics/decide_diagnostic.cpp` and each with tests and docs:
- `decide_frame_limit` (`physical_decide_frames.cpp:82`)
- `decide_mip_gap` (`physical_decide.cpp:6139`)
- `decide_l0_tolerance` (`decide_rewrite_norm_in.cpp:141`)
- `decide_direct_solve` (`direct_rule_driver.cpp:46`)
- `diagnose_decide_*` (via `GetDecideDiagnosticParams`)

The only switches with no doc are the evaluator-inject one, status markers and the solver-log one.

## 6. Duplicated code

`EvaluateConstraints` is `physical_decide.cpp:3903-4688` (786 lines in one function). `EvaluateObjective` is `:4689-4947` (259 lines). Yes, they repeat the same logic per clause shape.

**Shapes handled as separate branches in `EvaluateConstraints`:**
- Row-to-group assignment:
  - aggregate or easy MIN/MAX (`lhs_is_aggregate || was_minmax_easy`), with a `group_key`
  - the same, with no `group_key`
  - plus an overlay for `per K` naming classes
  - `scope.per == ONE_ROW_PER_CLASS`, with and without a signature
  - `when` only
  - plain per-row
- Right-hand side: constant (`BOUND_CONSTANT`), per-row data, the admitted-ends pair (`admitted_lower`), `tightest_frame_bound`, plus the `rows_in_a_class` sub-case.
- Terms: linear (`lhs_terms`), bilinear, quadratic groups (with a `scale` block).
- Special cases: the `<>` AVG hoist; MIN/MAX membership (`minmax_agg_type`).

**Shapes in `EvaluateObjective`:** a linear bucket, a quadratic bucket, bilinear terms, and `group_avg` for each of the three.

Near-duplicate blocks (15+ lines or repeated 3 or more times):

| where | what repeats | est. lines saved |
|---|---|---|
| `physical_decide.cpp:3964-3988`, `:4740-4760`, `:4800-4815` | Per-term `when` slot gathering (set `avg_scale`, push a slot or mark "unfiltered part"). The constraint side has 3 copies (lhs, bilinear, quadratic) and the objective side 2. | ~45 |
| `:4048,1562,4606` | `read = scoped_kept AND term.mask` built separately for linear, bilinear and quadratic terms. | ~15 |
| `:4075-4082` (constraint linear), `:4630-4638` (quadratic), `:4866-4876` (objective), `:1617-1625` (bilinear) | Inline copies of `MaskCoefficientColumn` (`:1264`), which already exists and is used elsewhere (l.1264, 5298, 5557+). | ~35 |
| `:4056-4071`, `:4610-4628`, `:4841-4862`, `:1596-1613`, plus the shared `EvaluateTermColumns` at `:4955-4986` | Five copies of "executor over coefficient expressions, window loop, `ExtractDoubleColumn`, `SyncSize`". Only `EvaluateTermColumns` is a helper. | ~60 |
| `:4553-4587` (squared-factor scale) vs `EvaluateQueryWideScale` (`:5016-5050`) | Evaluate a query-wide scale: one window, NULL check, finite check, divides-by-zero check. | ~25 |
| `:4195-4209`/`:5557-5614` and `:4510-4538` | Qualifier de-dup loops. Constraint side: lhs, bilinear and quadratic loops (`:4274-4291`). Objective side: three more in `BuildSolverInput` (`:5545-5580`). | ~40 |
| `:4500-4538` and `:5583-5614`, plus `:4925-4944` | AVG-scale loops in three term kinds on both sides, and the three `group_avg` loops. | ~40 |
| `:4651-4668` | Absent-read marking loops for lhs, bilinear and quadratic. Objective side repeats it at `:4772-4788`. | ~20 |
| `:1476-1490` (`CountTupleTermsOnce`) vs `:4895-4907` | First-kept-row-then-count-once block written twice. | ~10 |
| `RegisterEvaluation` (`:2448-2587`) vs the read sites it mirrors | A second traversal listing every expression each clause shape reads (constraints, objective, composed, value links, frames, signatures, scope checks). A new clause shape must be added in both. | structural, not counted |
| `:856-880` (`BuildSignatureIds`), `:1080-1130` (`NameVariants`), `:3557-3600` (`DescribeVariantDifference`) | Three switches over `DecideSignatureFieldKind` (VALUE, BY_KEY, FRAME, DECISION). | ~40 |
| `physical_decide.cpp:1728-1790` vs `src/planner/operator/decide/logical_decide.cpp:376-428` | `ParamsToString` is the same 45 lines in both operators. | ~45 |
| `r1_continuous_knapsack_rule.cpp:764-779`, `:1359-1375`; `a1_separable_box_rule.cpp:407-436` | Near-clone blocks (structural match, not read in detail). | unknown |

Subtotal: ~375, but the whole band is a refactor, not a deletion.

## 7. Comments describing something that does not exist on this branch

- `deck` appears in 50 comments in `src/`, citing page numbers ("deck p. 99", "deck §3", "deck pp. 77-119").
  - `context/docs/README.md:14` says the deck is `context/New_syntax.pdf`.
  - That file is not tracked on this branch, so none of those citations can be checked.
- `decide_row_store.hpp:9-23`, `physical_decide.hpp:308` and `physical_decide.cpp:2444-2447, 787` describe the evaluation as the deck's "annotated tree", with a "top-down construction" and a "bottom-up evaluation". The code is the flat store. No tree evaluator exists. `decide_evaluator.hpp:7,23-24` ("the deck's algorithm built beside it", "side plans") points at the unbuilt one. `physical_decide.cpp:3069` throws "the tree evaluator is not built yet".
- Comments naming identifiers that do not exist anywhere in `src/` or `test/`:
  - `linearization_minmax.cpp:893` `EmitComposedMinMaxOuter`
  - `solver_input.hpp:679` `ForEachInlinedColumn`
  - `r1_continuous_knapsack_rule.cpp:794` `AggregatePlanBuilder::LimitBounds`
  - `logical_decide.hpp:623` `DecideOptimizer::RewriteComposedMinMaxObjective`
  - `ilp_model_builder.cpp:835` `ValidateDecideNoStrictComparisonOnReal`
  - `physical_decide.hpp:150` `RawConstraints`
  - `linearization_not_equal.cpp:398-399` `ReduceAggregateRhsPerGroup (physical_decide.cpp)`
  - `linearization_bilinear_abs.cpp:344` and `logical_decide.hpp:462` `EvaluatedConstraints`
  - `direct_source_reader.hpp:26` `CopyIsCheaper`
  - `physical_decide.cpp:2339` `GetProcessMemoryInfo`
  - `decide_canonicalizer.hpp:56` `Binder::PlanSelectNode` (the real function is in `plan_select_node.cpp`)
- `physical_decide.cpp:1790-1793, 2436-2442, 3411-3418`: banner comments for phases whose code is elsewhere (see table 1).

## Totals (estimated removable source lines)

| group | lines |
|---|---|
| Evaluator-switch and verify scaffolding | ~436 |
| Dead code (tables 1 and 3, without the scaffolding) | ~265 |
| Duplication / refactor (table 6; consolidation, not pure deletion) | ~375 |
| Duplicate-fact fields (table 4) | ~60 |
| **Total** | **~1,130 lines** |

Optional on top of that, in tests and tooling:
- `test_evaluator_verify.py`: 93 lines.
- The generic suite-log compare tooling: ~860 lines.

## Suspected but not confirmed

- Whether `TermFilterState::has_filter` can really equal `!mask.empty()` in every case (the 0-row case is unchecked).
- `ClassSet::Kind::WHOLE_CLAUSE`: whether the render path treats it differently from `KEPT_ROWS`.
- Whether `slot_at` or `DecideRowWindows::resolved` is needed beyond `slot_hash`.
- Constant-argument parameters: I found only `keep_singleton_rows` (always `true`) and `ExecuteOnReadRows` (`row_offset == 0`, `rows != nullptr`). I did not run an exhaustive scan of all callers.
- Functions called only from dead code (transitive deadness) were not traced, beyond the scaffolding.
- Dead-code coverage in the planner, binder, canonicalizer and optimizer rewrite files rests on the identifier-count sweep plus a write-only-field heuristic. I did not read those files line by line. The sweep misses a function with a declaration, a definition and one call that is itself dead.
- The `anr.*` profile counters are read only by `benchmark/decide/anr_scaling.py` and `test_suite_log_compare.py`. I did not verify those readers run.
- Compiler warnings (unused static, unused member) were not consulted, since I did not build.
- The near-clone blocks in `r1_continuous_knapsack_rule.cpp` and `a1_separable_box_rule.cpp` appear structural only.
