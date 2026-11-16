<!-- Full report of a review agent, saved 2026-11-16. Model output, not verified unless the main report (../handoff.md) marks a finding as re-checked. -->

# Audit of Sami-OPA (tip 7837e300be) for dead, vestigial and duplicated DECIDE code

Read-only. I did not edit, build or test. No candidate is ruled out by anything but the greps below.

Method: I built a token and call-graph index over `src/` and `test/` with throwaway Python in the scratchpad. That covered references, enclosing functions and call-site arguments for the DECIDE scope. Every item below was then re-grepped by hand across `src/` and `test/`.

One caveat on the index. It misses operator() and class-scope uses. Several false positives were removed by reading the code (for example `UnboundedCandidateProvider::GetRowCandidates`, which is live).

Short answer: the suspicion holds. The tree evaluator was built with a second copy of its own facts (the node algebra), and the flat evaluator left behind unused code, single-implementer callback interfaces and about 120 comments that define behaviour by "what flat did". Of those 120 comment lines, ~110 are in the tree files (listed in section 6).

## 1. Defined but never called, or called only from dumps

Zero references anywhere in `src/` or `test/`, apart from the declaration and definition:

| Where | What | Evidence | Lines |
|---|---|---|---|
| `execution/operator/decide/physical_decide.cpp:893-932` | `struct DecideEvaluationFields` and `SwapWith` | Name occurs once in the repo. It swaps the `gstate` fields a "second run" would fill, a leftover of the shadow run. | 40 |
| `execution/operator/decide/decide_value_extraction.cpp:117,161` and `.hpp:53,57` | two overloads of `ExecuteOnReadRows` | Only the 161 overload calls the 117 one. Nothing else does. | ~42 |
| `decide_value_extraction.cpp:910`, `.hpp:320` | `DecideListRowMembers` | Zero callers. Its helper `DecideListMember` is still used by `DecideListFrameMembers`. | ~20 |
| `execution/operator/decide/physical_decide_frames.cpp:709-723` | `AppendPayloadLeaves` | Only self-recursive. | 16 |
| `include/duckdb/decidb/formulation/solver_input.hpp:50-212` | the whole `CoefficientColumn::Kind::SparseMasked` mode | Its only constructor, `MakeSparseMasked`, is never called. That makes `IsSparseMasked`, `MaskRow`, `ScaleRow`, `FromVector`, `sparse_indices`, `sparse_value` and the SparseMasked branches in `Get`, `EnsureDense`, `AssignScalar`, `AssignDense` and `AllIntegral` unreachable. | ~55 |
| `decide_frame_map.hpp:109` | `DecideFrameMap::PeerSize` | Zero callers. | 4 |
| `decidb/formulation/ilp_linearization.cpp:83-91`, `ilp_linearization_internal.hpp:31` | `PinGlobalAux` | Declaration and definition only. | 12 |
| `include/duckdb/decidb/formulation/ilp_linearization.hpp:100-110` | `LinearizeMinMaxIndicators` | Declared with a doc block but never defined. | 11 |
| `optimizer/decide/decide_rewrite_bilinear.cpp:75-82` | `GetSingleDecideVarIdx` | Zero callers. | 8 |
| `optimizer/decide/direct/r1_continuous_knapsack_rule.cpp:954-960` | `ReadOnBudgetRows` | Zero callers. | 7 |
| `optimizer/decide/direct/direct_plan_fragments.cpp:285-307`, `.hpp:233-238` | `DirectCheckRowBounds` | Name occurs only in the declaration, the definition and one comment. | ~30 |
| `execution/operator/decide/physical_decide.cpp:171-194,1325-1329` | `VerifyCountedOnce` (DEBUG only) | Its `row_group_ids` argument is always an empty vector. `CountsOnce` stays live. | 25 |
| `physical_decide.hpp:57`, `physical_decide.cpp:212-215` | `PhysicalDecide::decide_variable_map` | Written in the constructor and never read. | 6 |
| `decide_global_sink_state.hpp:36,227` | `DecideGlobalSinkState::canonicalizer`, plus the `context` member | Constructed but never referenced as `gstate.canonicalizer` or `gstate.context`. | ~5 |
| `decide_global_sink_state.hpp:278-280` | `frame_column_refs` | Referenced only by the dead `DecideEvaluationFields`. | 3 |
| `decide_global_sink_state.hpp:304-306` | the one-argument `ReadVariable(const DecideTerm&)` overload | Every caller uses the two-argument form. | 3 |

Referenced only from debug dumps. These are test instruments (env vars in section 5), not unused code:

| Where | What | Lines |
|---|---|---|
| `planner/decide/decide_tree.cpp:2891-2988` | `DecideTree::ToString` | 98 |
| `planner/decide/decide_tree_compile.cpp:706-886` | `BodiesToString`, `ClauseLines`, `ObjectiveLines`, `ProgramLines`, `RoleName`, `BlockName` and the other line helpers | ~180 |
| `execution/operator/decide/decide_descent.cpp:2295-2465` | `DecideDescent::Step` and `Dump` | ~170 |
| `execution/operator/decide/decide_tree_columns.cpp:901-922` | `DecideTreeColumns::Dump` | 22 |
| `execution/operator/decide/physical_decide.cpp:853-861,887-889` | env wiring for the dumps | ~15 |

Together about 485 lines, removable only if the 8 test files that read them are rewritten. `DecideNode::hidden` (written 7 times) and `l0_auto_m` are read only by `ToString`.

I found no function referenced only from test code. The `DirectSourceCopyIsCheaper` hit was a false positive; it is called at `direct_source_reader.cpp:67`.

## 2. Fields written but never read, or read only to cross-check

| Where | Field | Evidence | Lines |
|---|---|---|---|
| `planner/decide/decide_tree.hpp:254-256`, `decide_tree.cpp:1580-1582` | `DecideNode::repair_rows`, `minmax_easy`, `tightest_bound` | Written once each, read nowhere. | 6 |
| `decide_tree.hpp:55-59,177`, `decide_tree.cpp:1737,2842-2846` | `DecideNode::child_reads` and the struct `DecideTreeRead` | Written at 1737. The only read is a `Validate` check that `column == input_column_count + node`, which is how `Read()` just built it. | ~22 |
| `decide_tree.hpp:220` | `l0_auto_m` | Read only by `ToString`. | 2 |
| `physical_decide.hpp:131`, `plan_decide.cpp:147` | `PhysicalDecide::flat_objective_is_easy` | Copied from the logical op and never read. | 3 |
| `decide_model_emitter.cpp:1020`, `solver_input.hpp:367` | `rows_stand_with_their_class` | Written once. The only other mention is a test comment. | 2 |
| `ilp_model.hpp:324`, `solver_input.hpp:766` | `indicator_col` | The builder and linearizer write it. Only `test_decidb_diagnostic_engines.cpp` sets or reads it. No diagnostics source reads it. | 6 |
| `algebra->owner`, `->is_easy`, `->value` | `DecideAlgebraNode` fields | `owner` is read only for an "is own definition" test that `value_nodes` can answer. `value` is read at one site (`decide_model_emitter.cpp:1927`) that falls back to `node.rhs`. Treated under pair A. | – |
| `logical_decide.hpp:531` | `ComposedMinMaxTerm::Kind` | `SUM_KIND` is only assigned, then compared against `MINMAX_KIND`. It is derivable from `agg_name`. | ~5 |

## 3. Parameters every caller gives the same value

| Function | Parameter | Always |
|---|---|---|
| `PhysicalDecide::EvaluateTree` (`physical_decide.cpp:831`, `.hpp:299`) | `bool silent` | `false`. This leaves the `!silent` guards at 853-861 and 887, and the `silent` flag of `ReportSkippedInstances` (255, 984, 996), unreachable. |
| `PhysicalDecide::EvaluateTree` | `unique_ptr<DecideDescent> &descent` | `gstate.descent` |
| `PhysicalDecide::FormulateElasticModel` (`physical_decide.cpp:1789`) | `keep_singleton_rows` | `true` |
| `decide_canonicalizer.hpp:531` `BuildAdditive` | `bool negate` | `false` (4 calls) |
| `decide_descent.cpp:35` `DescentHash` | `parent` | `0` (2 calls) |
| `decide_descent.cpp:165` `DescentRowValues` | `begin` | `0` |
| `decide_model_emitter.cpp:1807` `ObjectiveTerms` | `formula_offset` | `0` (2 calls) |
| `decide_rewrite_frames.cpp:63` `CollectRangeSigns` | `sign` | `1` (2 calls) |
| `decide_value_extraction.cpp:1052` `TakeTightestFrameBound` | `grouped` | `true` |
| `linearization_bigm.cpp:31` `DecideRowTermRange` | `skip_idx` | `INVALID_INDEX` |
| `solver_input.hpp:163` `AssignDense` | `init` | default (10 calls) |
| `decide_source_provenance.hpp:115` `CollectDecideExpressionStrings` | `sources` | `nullptr` (4 calls) |
| `direct_plan_fragments.hpp:89` `DirectTightestBound` | `reads` | default (2 calls) |
| `decide_key_map.hpp:25` `DecideKeyMap` | `memo` | not constant; callers pass `numbered` |

Removing these saves about 40 lines.

## 4. The same fact stored twice

**Pair A (largest): `DecideAlgebraNode` versus the term records**
- The algebra (`DecideNode::algebra`) mirrors `DecideTerm`, `DecideBody` (`compiled.clause`, `objective`, `composed`, `members`, `linear`, `products`, `squares`, `rhs`), `DecideTermSite` and `tree.value_nodes`.
- Written: `BuildAlgebra` and the `Algebra*` helpers, `decide_tree.cpp:732-1136` (~405 lines).
- Read: `decide_model_emitter.cpp:29-310` (14 helper functions, ~280 lines) plus about 125 use sites. `physical_decide.cpp:68-139` reads it for the objective sense, composed-objective test and extremum type. `Validate` (`decide_tree.cpp:2402-2724`, ~320 lines) cross-checks it, and `ToString` prints it.
- In the emitter, `AlgebraTermProgram` and `AlgebraProductProgram` do not supply the answer. They verify that sign, variable, `source_term`, `program_node` and `program` equal the `DecideTerm` and `site` values, then return `coefficient.program`, which they have already checked equals `site.program`.
- The `read_variable` out-parameter is checked equal to `term.variable_index`.
- `AlgebraTermValueNode` is `algebra_value_nodes[variable]`. That vector is `tree->value_nodes` plus the function nodes, and it is rebuilt a second time in `Validate` (`decide_tree.cpp:2361-2370`).
- The F-node fields (`function_kind`, `abs_needs_bigm`, `abs_indicator_var`, `l0_m`, `product_bool_var`, `product_other_var`, `definition_relation`) are copied into the algebra (`sign`, `scale`, children) at `decide_tree.cpp:1092-1131`. `Validate` then checks that they match, `decide_tree.cpp:2637-2723`.
- `quadratic_sign` lives in `DecideObjective::quadratic_sign` and in `square->scale`. The emitter checks them equal (`decide_model_emitter.cpp:1757`) and then uses the algebra copy.
- Nothing the emitter reads exists only in the algebra, except tree-value producers, which `value_nodes` supplies.
- Removable: the algebra, its builder, the emitter helpers and the algebra half of `Validate`. About 900 lines net (builder ~405, `Validate` ~320, emitter helpers ~280 and use sites ~100–150, header ~25, less ~60 lines to read program, variable and producer directly).
- The algebra was added in the last ~60 commits, so it may be intended as the future source of truth. If so, `compiled.*` is the redundant half instead. Either way one of the two can go.

**Pair B: clause identity in three places**
- `DecideNode::{comparison_type, source_clause_id, removal_group_id, defined_var}`, `DecideNode::facts` (a term-free `DecideConstraint`) and `compiled.clause` (the same struct with terms).
- `ReadDecideClauseFacts` fills `facts` at `decide_tree.cpp:39-119`. The builder copies the same values into the node fields at 324-332, 468-470, 547, 1578-1586, and syncs `facts->defined_var` by hand at 1601, 1620 and 1669.
- The algebra check at `decide_model_emitter.cpp:54` compares `expression_type` against `compiled.clause->comparison_type`.
- Could be one record. ~40 lines.

**Pair C: `LogicalDecide` side tables versus their retained copies**
- `bound_aggregates` (cleared at `direct_row_checks.cpp:135`, `direct_bound_tables.cpp:194`) is copied into `retained_bound_aggregates` for the tree at `direct_row_checks.cpp:119`.
- `retained_row_checks` is copied from `check.invalid` at `direct_row_checks.cpp:105`.
- `logical_decide.hpp:734,747` still call this "transition" state for the "flat oracle". Plumbing is about 60 lines.

**Pair D: the ABS maximize link in three forms**
- `LogicalDecide::abs_maximize_links` (written `decide_rewrite_abs.cpp:469`), then `DecideNode::abs_needs_bigm` and `abs_indicator_var` (`decide_tree.cpp:720-723`), then `SolverInput::abs_maximize_links` (`physical_decide.cpp:1268-1274`).
- The middle copy exists only because the tree is "shadowing" the plan-time rewrite (comments at `decide_tree.hpp:229,232,308,318`). About 30 lines.

**Pair E: single-implementer callback interfaces**
- `DecideFrameRead` (`decide_frame_map.hpp:203-205`) is read by `ComputeDecideFrameColumns` and `SkipDecideFrameNullReads`. It has one provider, `decide_tree_columns.cpp:755`, though its comment still says "flat's one pass, or the tree's programs".
- `DecideVariantReader` and `DecideFrameReader` (`decide_value_extraction.hpp:337-373`) each have one implementer, `decide_model_emitter.cpp:520-556`. Their comments still say "flat from the rows, the tree from its root's entries".
- Inlining them saves about 70 lines.

**Pair F: `ReducerValueLink`**
- `LogicalDecide::ReducerValueLink` becomes `ReducerValueLinkSpec` through `gstate.evaluated_value_links` and the emitter's `ValueLinks`.
- This is a staged hand-off. I did not find a redundant copy.

## 5. `DECIDB_*` switches and settings read in this code

| Name | Read at | Used by tests or docs |
|---|---|---|
| `DECIDB_DESCENT_CHUNK` | `physical_decide.cpp:316` | `test_descent_merge.py`, `test_one_pass_tree.py`, `test_one_pass_literal.py` |
| `DECIDB_DESCENT_SPLIT` | `physical_decide.cpp:366` | `test_descent_merge.py` |
| `DECIDB_DUMP_STEPS` | `physical_decide.cpp:325` | `test_one_pass_literal.py` |
| `DECIDB_DUMP_BODIES` | `physical_decide.cpp:853` | `test_tree_bodies.py`, `test_tree_emitter.py`, `test_null_values.py` |
| `DECIDB_DUMP_TREE` | `physical_decide.cpp:856` | 8 test files |
| `DECIDB_DUMP_CONTAINERS` | `physical_decide.cpp:859` | 3 test files |
| `DECIDB_DUMP_VALUES` | `physical_decide.cpp:887` | `test_tree_emitter.py` only |
| `DECIDB_STATUS_MARKERS` | `physical_decide.cpp:2111` | `decidb_cli.py`, `test/decide/README.md`, `test_suite_log_compare.py` |
| `DECIDB_BENCH` | `physical_decide.cpp:752` and `decide_optimizer.cpp:87` (two readers) | `benchmark/decide/*`, `Makefile`, `test/decide/README.md` |
| `DECIDB_TREE_INJECT` | `decide_tree_columns.cpp:873` | `test_one_pass_execution.py` |
| `DECIDB_EVALUATOR_INJECT` | `decide_tree.cpp:1154` | `test_tree_bodies.py` |
| `DECIDB_FORCE_SOLVER` | `decide_answer_choice.cpp:61` (message text only; the read is in `ilp_solver.cpp`) | many tests |

Outside this scope: `DECIDB_NATIVE_CONSTRUCTS` is used by many tests, and `DECIDB_PROFILE` is used by benchmark and test files. Neither is read in the scoped code.

One setting, `decide_direct_solve` (`direct_rule_driver.cpp:46`, error text in `decide_answer_choice.cpp:60,69`), is exercised by `test/decide/run_direct_matrix.sh` and many tests.

`DECIDB_DUMP_VALUES` and `DECIDB_DUMP_STEPS` each have a single test.

## 6. Comments and names that depend on deleted code

I counted lines containing "flat" that mean the deleted evaluator. I excluded "flat column space", "flat index" and the like. Total is about 118 lines.

By file:

| File | Lines |
|---|---|
| `decide_model_emitter.hpp` | 15 |
| `decide_model_emitter.cpp` | 14 |
| `decide_tree.hpp` | 14 |
| `decide_tree_compile.cpp` | 14 |
| `decide_tree.cpp` | 13 |
| `decide_descent.cpp` | 9 |
| `decide_tree_columns.cpp` | 8 |
| `decide_error_ledger.hpp` | 5 |
| `decide_tree_columns.hpp` | 5 |
| `physical_decide.cpp` | 3 |
| `decide_rewrite_minmax.cpp` | 4 |
| `decide_descent.hpp` | 2 |
| `decide_value_extraction.hpp` | 2 |
| `physical_decide.hpp` | 2 |
| `logical_decide.hpp` | 2 |
| `decide_frame_map.hpp`, `decide_tree_terms.hpp`, `decide.hpp`, `decide_source_provenance.cpp` | 1 each |

Separate "phase" and "P" labels:
- `PHASE n` / `sub-phase` banners: 14 lines in `physical_decide.cpp`, 6 in `physical_decide.hpp`, 1 in `decide_global_sink_state.hpp` (the old evaluator's phase numbering).
- Five section banners have no code under them: `physical_decide.cpp:304-307` ("Multi-variable per-row constraint helpers"), `741-747` (PHASE 1.5 and 1.4), and `951-957` (sub-phase 1 and PHASE 1.6).
- `P9` appears in `decide_tree.hpp:229,308,318`. P9 refers to a migration step ("P9 lowers these nodes in the model emitter"). The emitter already lowers ABS F nodes.
- Stale "transition" wording: `physical_decide.hpp:37` ("captured before flat frame lowering during migration"), `logical_decide.hpp:734,745,747`, `decide_tree.cpp:681` ("transitional envelope roots"), `decide_tree.hpp:232`, `decide.hpp:648` ("shadow F node").

The name `flat_objective_agg` / `flat_objective_is_easy` / `flat_agg` / `flat_is_easy` (31 references) now means "non-nested MIN/MAX objective". It was named against the deleted evaluator.

Ten most load-bearing:
1. `decide_error_ledger.hpp:56-66`, `DecideBlock` and `DecideBlockExpression`, defined as "where flat reads an expression (`PhysicalDecide::Evaluate`)". That function exists in no code. The block ordering and window arithmetic cannot be understood without flat.
2. `decide_tree.hpp:158-162`, `DecideBody::phase` and `order`: "the rank flat's evaluation gives the stage-05 list (`PhysicalDecide::Evaluate`)".
3. `decide_tree.hpp:263-275`, `key_block`, `filter_group` and `filter_part`: all defined as "where flat reads its key / its `when`".
4. `decide_tree.cpp:1231-1233,1273-1318`, `KeyedMappingOrder` and `ErrorRanks`: "Flat resolves each auxiliary's absence in turn", "Flat reads a helper's rows". The rank table is flat's evaluation order.
5. `decide_descent.cpp:296`: "flat numbers a key's groups (`EncodeSortKeys`)". `EncodeSortKeys` exists nowhere in code.
6. `decide_descent.cpp:582,902-903,1524-1530,1564,1618`: where each read ranks is "where flat reads it".
7. `decide_model_emitter.cpp:617,1079,1101`: "numbered as flat numbers them by value". The group numbering rule is not stated anywhere else.
8. `decide_tree_columns.cpp:339,429,455,579`: error order inside an owner's evaluation is "flat's order" and "which flat's window raises first".
9. `decide_tree_compile.cpp:129,182,252`: "as flat's `repair_group_id` numbers them", "by flat's own rules (`ToChunkReferences`)", "as flat's `NameVariants` names". The numbering is defined by a deleted implementation.
10. `decide_value_extraction.hpp:339,357` and `decide_frame_map.hpp:203`: three interfaces described as "flat from the rows, the tree from its entries".

Almost all of these describe error precedence. Tests (`test_error_precedence.py` and others) pin that order. The code is live, but its specification exists only as references to a deleted evaluator.

## 7. Duplicated code (near-identical, 15+ lines unless noted)

| Locations | Lines | Note |
|---|---|---|
| `physical_decide.cpp:246-302` and `planner/operator/decide/logical_decide.cpp:412-468` | ~45 | `ParamsToString` bodies. Differences are `run_time_accounts` and the composed-objective test. |
| `physical_decide.cpp:2429-2503` | ~45 | Five readback loops that differ only in output type (int32, int64, bool, double, default int64). One template would do. |
| `decidb/formulation/ilp_model_builder.cpp:719-791` and `1229-1290` | ~45 | Quadratic row/objective products; `struct VarCoeff` is declared twice. |
| `decide_tree.cpp:926-946` and `976-996` | ~20 | Bilinear algebra node (disappears with Pair A). |
| `decide_tree.cpp:2497-2507` and `2568-2578` | ~10 | Validate checks. |
| `decide_tree.cpp:476-490`/`528-542` and `608-621`/`704-717` | ~25 | F-node setup. |
| `optimizer/decide/decide_linear_form.cpp:922-943` and `955-976` | ~20 | |
| `decide_model_emitter.cpp:921-933` and `1379-1391` | ~12 | |
| `decidb/formulation/linearization_not_equal.cpp:218-239` and `519-542` | ~20 | |
| `decide_source_provenance.cpp:1048-1115` | 6 repeats of one 16-line pattern | |
| `optimizer/decide/decide_rewrite_*.cpp:9-38` | – | Only include blocks; not real duplication. |

Only about 230 lines are real duplication, because exact 15-line repeats are rare. Exact-text and identifier-normalised scans found nothing larger.

## Estimated removable lines

| Group | Lines |
|---|---|
| Section 1 dead code, sections 2 and 3 | ~370 |
| Pair A (algebra) | ~900 |
| Pairs B–E | ~200 |
| Real duplication (section 7, less the algebra overlap) | ~210 |
| **Total without touching the dumps** | **~1,700** |
| Dump instruments, if the 8 test files that read them change | ~485 |
| **Total including the dumps** | **~2,200** |

Pair A is the only one that is a design decision. The rest is mechanical.

## Suspected but not confirmed

- The 530-line `DecideTree::Validate` (`decide_tree.cpp:2359-2889`). Apart from the algebra and `child_reads` checks it is defensive. I did not judge which branches are unreachable.
- The error-rank machinery (`route_rank`, `value_rank`, `clause_rank`, `when_rank`, `DecideProgram::{block, group, position, part}`, `DecideBlockExpression`). It is live and tested, but it exists to reproduce flat's error order. Whether the order is still wanted as specified is a product question.
- `DecideNode::dependencies` versus `children`, and `tree.definition_nodes` versus `function_nodes`. They overlap, but I could not prove one is derivable.
- Field analysis covered names with 60 or fewer occurrences. Common names such as `rows`, `text` and `key` were checked only for the `DecideNode` fields.
- The direct-solve files (`optimizer/decide/direct/*`) were scanned only for dead functions and constant parameters. Rule-internal dead code there is untested.
- `LogicalDecide::Kind::SUM_KIND` and `SolverInput::ClassSet::Kind::WHOLE_CLAUSE` are written but never compared. They are read through `switch` in the renderer, so I treated them as live.
- The decision labels (`D70`, `D88` and others) and stage numbers ("stage 05/06/08", plan labels `L0`–`L6`) in comments reference docs. I did not check them against `context/docs/`.

Key files, all under `/Users/hatim/Code/decidb/.claude/worktrees/sami-opa/src/`:
- `planner/decide/decide_tree.cpp`
- `execution/operator/decide/decide_model_emitter.cpp`
- `execution/operator/decide/physical_decide.cpp`
- `include/duckdb/planner/decide/decide_tree.hpp`
- `include/duckdb/decidb/formulation/solver_input.hpp`
- `include/duckdb/common/decide_error_ledger.hpp`
- `include/duckdb/execution/operator/decide/decide_global_sink_state.hpp`
