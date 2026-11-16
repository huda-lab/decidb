<!-- Full report of a review agent, saved 2026-11-16. Model output, not verified unless the main report (../handoff.md) marks a finding as re-checked. -->

# Test-quality audit: Sami-ANR (A) vs Sami-OPA (B)

Read-only. I used `git diff`, `git show`, `git log` and file reads. I ran no build, no pytest, and touched nothing under `build/` or `results/`.

Caveats:
- Every "passes" or "fails if reverted" claim below comes from commit messages. I did not run or revert anything.
- Item counts for parametrized tests are from dict/list sizes. They are approximate.
- Per-file classification counts for A's 20 largest added files come from a regex over each test function's body. Treat them as indicative, not exact.

## Headline

- B keeps A's oracle suite almost untouched. Its additions are mostly stored snapshots and internal-format pins, not independent checks.
- Two user-visible changes had their expectations flipped to match new output with no stated reason (see "Unjustified or weakened changes"):
  - EXPLAIN stopped printing the rows generated for ABS and L0.
  - The "one pass" counter assertion was reduced to "greater than 0".
- B fixed four of A's five known fuzz defects (D1, D2, D4, D5) with regression tests. D3 is still open.
- Only 2 of B's 141 new test functions are checked against an independent oracle.
- Nothing in B's suite runs the tree evaluator on more than one thread. A crash on multi-threaded input was found and fixed in the last commit.

## Q1. Existing test files B modified (relative to A)

Labels: (a) new test, (b) justified by a deliberate fix, (c) expectation changed with no evident justification, (d) weakened, narrowed, pinned or deleted.

| File | Change | Class | Evidence |
|---|---|---|---|
| `test_abs_linearization.py:1958-1975` | Model size pin changed from (9,11)/(9,14) to (7,8)/(7,10). Id changed from `abs-over-a-reducer-is-per-row` to `...-one-auxiliary-per-group`. | (b) | Commit 9014a4c3d0 fixes ABS over a reducer's helper being per row. It cites the deck's "one value per class" and fuzz nests seed 5261, and says the new tests fail without the fix (9 vs 10). The oracle-judged answer check stays. |
| `test_anr_nested.py` | Exact model rows added to F1; 2 new oracle-judged tests (`...is_one_value_per_class`, `...nested_mean...`); docstring edits | (a)/(b) | Same commit 9014a4c3d0. The row pins make the test stronger. |
| `test_anr_inlining.py:168-248` | 4 new objective model pins (hand-stated col/row dumps) | (a) | Commit a4263883ff admits the slices that added them "no model changed (every test they added passes without them)". So they are pins on existing output, not tests of the added code. The quadratic one needs Gurobi. |
| `test_bilinear.py:2239-2339` | New: product floor (4 cases), lowest floor, reducer floor, non-negative decision | (a)/(b) | Commit e19ed66d5a. 6 of 7 items are judged against exhaustive `judge_rows`; the 7th checks the error text. Good tests. |
| `test_collated_keys.py` | Helper extracted (same assertion). 5 new tree-naming tests (least spelling, bound-reducer fold, label) | (a) | They assert user-facing message text, hand-stated. |
| `test_error_precedence.py` | New pairs and sub-steps (`HELD_CONSTANT`/`HELD_ROW_ERROR`, `HELD_TERM_ERROR`, one-frame routed/listed/checked, scope-check `when`, timeline order). `_limited` helper. | (a) | Additions only. Commit 6b372561ec makes the tree rank errors in flat's order. This file does not read `_expected.py`. |
| `test_invalid_value_message.py:255-278` | `TREE_WRITTEN` (3 cases) | (a) | Hand-stated message text. |
| `test_scope_explain_diagnose.py:239-249` | New test `an_easy_max_with_its_own_when...` | (a)/(b) | Commit d1d0ef49be. It guards against over-correcting the D1 fix. |
| `test_fuzz_smoke.py` | Seeds 453 (nests, D1) and 37 (frames, D4) added. `test_bounds_find_a_group_with_no_class` deleted. | (a); deletion (b) | The deleted test covered `holds_orphan_group`, which was removed in dd356dd64e. |
| `test_like_term_collection.py`, `test_minmax_body_definition.py`, `test_quadratic.py` | Docstring/comment renames only | n/a | No assertion changed. |
| `golden/baseline.{gurobi,highs}.dump` (lines 1764 and 1713) | `col 4 ub=40` became `ub=25` | (b), unexplained | Commit 3964b76f26 ("record tighter implied helper bound") has no body. I checked by reading: rows 0-1 have rhs=25 with unit coefficients on non-negative columns, so 25 is a valid implied bound. It is the only golden line that changed. |
| `golden/capture.sh` | `DECIDB_EVALUATOR=flat` pin removed | (b) | Flat is deleted. |
| `test/common/test_decidb_helper_inlining.cpp` | All 11 `InlineReducerValueLinks` calls gain a third `{}` | mechanical | Commit d16b7383f6. The new `minmax_values` parameter is never given a non-empty value in this unit test. |
| `test_null_values.py:222-242` | Profile counter `term_filters` replaced by a `DECIDB_DUMP_TREE`/`DUMP_BODIES` regex | (b), partly (d) | Commit 1ec281897d: the span no longer exists. The oracle optimum check stays. The test now pins a compile-time dump line instead of "no filter was read at run time". |
| `test_scope_reducer_by.py` (A:252-270) | Deleted `a_key_shared_by_a_decision_and_a_by_is_computed_once` | (b)/(d) | The test used flat's `extraction.group_ids` span (commit 19784f71b6). Its performance property is not re-asserted for the tree. |
| `test_explain.py:695-716` | See (c) list | (c) | |
| `test_scope_explain_diagnose.py:69,174,239-250` | See (c) list | (c) | |
| `test_one_pass.py:35-36` | See (d) list | (d) | |
| `test_direct_r1_continuous_knapsack.py:2226-2231` | See (d) list | (b)/(d) | |
| `_old_spellings.py:183-187` | See (d) list | (d) | |

### Unjustified or weakened changes

**(c) Changed to match new output, no stated reason**
1. `test_explain.py:695-716`, `test_abs_clause_shows_the_rows_it_became` renamed `..._shows_its_source_and_account`.
   - Before: `packed.count("abs(x-20)")+count("ABS(x-20)") >= 4` and `count(_REWRITTEN_MARK) >= 3`.
   - After: `"abs(x-20)-x" not in packed`, `"abs(x-20)+x" not in packed`, `count(_REWRITTEN_MARK) == 1`.
   - The test went from requiring the generated envelope rows in EXPLAIN to forbidding them.
   - The only commit that touches it is 017b1dfadf. It has no body.
2. `test_scope_explain_diagnose.py:69` (abs) and `:174` (l0-bound-is-derived).
   - Before: expected EXPLAIN rows `whenS.priority:abs(ship-20)-ship>=-20` and `whenS.priority:M*(ship<>0)-ship>=0.0`.
   - After: `None`, so the row check is skipped. The docstring now says "EXPLAIN is plan-time only".
   - Commits 017b1dfadf and 7f7747c1e8 ("tests: align scanner and L0 explain expectations"). Neither has a body.
3. `test_scope_explain_diagnose.py:239-250`, `..._accounts_for_the_objectives_rewrites_too`.
   - Dropped `↳whenS.priority:abs(ship-10)-ship>=-10`. Added three "not in text" assertions.
4. `test_tree_emitter.py:1482-1493`, `test_a_helper_read_once_is_written_where_it_is_read`.
   - Before: tree EXPLAIN ANALYZE output equal to flat's, ignoring timings.
   - After: the account is present and the envelope row is absent.
   - `accounts[0]`, the stored flat output, is still fetched and never used.
   - Commit 017b1dfadf.
5. `test_tree_emitter.py` stored `model-*.json` (5 files) changed in dd356dd64e.
   - Example: `ub=9` became `ub=5`, and `1e+30` became `20`.
   - The values are valid tighter implied bounds, and the commit names the D5 emitter references.
   - But these are "flat's recorded output" overwritten with the tree's output, so the stored value is no longer a flat reference.

**(d) Weakened, narrowed or deleted**
1. `test_one_pass.py:35-36`.
   - Before: `input_scans(...) == [1]`, which means one scan.
   - After: `len(rows)==1 and rows[0] > 0`.
   - The docstring still says "the evaluation never scans the buffer". Nothing asserts that. The `input_scans` counter was removed in f12d9f9180.
2. `test_direct_r1_continuous_knapsack.py:2226-2231`.
   - The loop `for limit, copied in (("32MB", False), ("4GB", True))` with `("direct_source" in plan) == copied` became `not in` for both limits.
   - The loop now tests one outcome twice. The memory-limit-dependent branch is gone (commits ca1dbe2c6c and 19784f71b6, since side plans were removed).
3. `_old_spellings.py:183-187`.
   - The removed-syntax scanner now returns `set()` for any text matching `\d+\s+[EGOF]\s+` and ` formula=`.
   - This is a case-specific exemption added so the pinned tree-dump fixtures in `test_one_pass_tree.py:~200-245` pass the lint.
4. `test_scope_reducer_by.py`: deleted test, see the table.
5. `test_null_values.py`: now pinned to a dump format, see the table.

### Exhaustive detail on Q1(d)
The only deletions of whole tests are `test_evaluator_verify.py`, `test_a_key_shared...is_computed_once` and `test_bounds_find_a_group_with_no_class`. The first and third go with deleted code. The second has no tree equivalent.

## Q2. Tests B added

Classes:
- **BL-O**: behaviour-level against an independent oracle.
- **BL-H**: behaviour-level against a hand-stated answer, model or message.
- **SR**: self-referential (stored earlier run, or the same engine compared with itself under a split).
- **IP**: pins an internal dump, counter, injection hook or plan-operator name.

Counts are test functions, with parametrized items in parentheses.

| File | Fns (items) | BL-O | BL-H | SR | IP |
|---|---|---|---|---|---|
| `test_tree_emitter.py` | 79 (~250; 259 stored records) | 0 | 2 | 72 | 5 |
| `test_tree_bodies.py` | 15 (35) | 0 | 0 | 0 | 15 |
| `test_one_pass_tree.py` | 11 (18) | 0 | 0 | 0 | 11 |
| `test_one_pass_literal.py` | 5 (~125) | 0 | 0 | 0 | 5 |
| `test_one_pass_execution.py` | 15 (~36) | 1 | 13 | 0 | 1 |
| `test_one_pass_noside.py` | 4 (6) | 1 | 0 | 0 | 3 |
| `test_one_pass_errors.py` | 2 (10) | 0 | 2 | 0 | 0 |
| `test_descent_merge.py` | 4 (13) | 0 | 1 | 3 | 0 (the 3 SR fns compare the internal containers dump too, 11 items) |
| `test_fuzz_defects.py` | 5 (13) | 0 | 5 | 0 | 0 |
| `test_decidb_descent_reads.cpp` | 1 case, 3 sections | 0 | 0 | 0 | 1 |
| Total | 141 | 2 | 23 | 75 | 41 |

Details behind the table:
- `test_tree_emitter.py`
  - 72 compare against `_expected.py` (`models()`, `reference()`, `_verified`, `_modelled_as_flat`).
  - 38 of those use stored model dumps, and 44 use stored stdout/stderr (10 use both).
  - The 5 IP tests are the four F-node `DUMP_TREE` regex tests at lines 88-146 and `test_a_covered_query_prints_each_dump...` at 313.
  - The 2 BL-H tests are `test_a_null_in_a_later_window...` and `test_a_helper_over_every_row_is_one_column`.
- `test_tree_bodies.py`: every test regex-matches `DECIDB_DUMP_BODIES`/`DUMP_TREE`. One uses the `DECIDB_EVALUATOR_INJECT=g1` hook and asserts INTERNAL error text. One also asserts a user-facing error.
- `test_one_pass_tree.py`: containers and tree dumps, including 3 verbatim `PINNED_DUMPS` (`formula=...` strings).
- `test_one_pass_literal.py`
  - L6 reach checks run over about 60 deck cases at chunk sizes 1 and 2048, plus step-dump "animation" replays.
  - All of it is read off `DUMP_CONTAINERS`/`DUMP_STEPS`.
- `test_one_pass_execution.py`
  - The BL-O test is `test_tree_values_reach_the_decks_model`. It uses `model.judge`, plus a pinned `dump_counts`.
  - The BL-H tests are answers "worked by hand" (nesting, signature, grouped counts, `<>` with NULL bound), plus EXPLAIN ANALYZE text and one timing-based interrupt test.
  - The IP test uses the `DECIDB_TREE_INJECT` hook.
  - The NESTING_CASES comment "The default suite runs flat." is stale.
- `test_one_pass_noside.py`: `test_generated_presence_tests_read_computed_children` (3 items) uses `model.judge`. The other three check EXPLAIN operator names, EXPLAIN text, or a tree-dump regex.
- `test_descent_merge.py`
  - Everything runs with `SET threads = 1` and `DECIDB_DESCENT_SPLIT`, which simulates threads by merging every n rows.
  - Comparison is whole-read vs split-read of the same engine, on the internal containers dump and the model dump.
  - It is a real metamorphic property, but it is self-referential. No oracle is involved.
  - Only the collated-group message ('B') is hand-stated.
  - The error-rank test uses 2048-row pieces.
- `test_fuzz_defects.py`: answers hand-worked, no oracle, by design ("with its answer worked by hand").

Stability if internals change again (for example model writing moves into the tree nodes):

| Group | Functions | What happens |
|---|---|---|
| Must be rewritten | 44 (41 IP + 3 containers-dump merge tests) | They read dump formats, node ids or program text. `test_tree_bodies.py` (all 15) dies if stage 05 lowering moves. |
| Fails on any change to emitted model text | 38 emitter tests with stored model dumps (112 model records) | Exact string equality on column/row order and numbering. The probable fix is re-recording from the new output, which is circular. |
| Should survive | about 59 (34 run-only stored, 23 BL-H, 2 BL-O) | Stored run output (messages, tables) breaks only if messages or ordering change. |

Roughly 31% of B's added functions must be rewritten and another 27% are at risk of mass re-recording. A's oracle files (about 90 of them) are unaffected by internals.

## Q3. Stored expectations (`tests/_expected.py`, `tests/expected/`)

- **What is stored.**
  - `model-*.json` (112): the full `DECIDB_DUMP_MODEL` text. `run-*.json` (147): returncode, stdout, stderr of one CLI run.
  - The key is sha1(`[kind, sql, env]`). `env` holds the wrapper env minus solver, direct-solve and verify-serializer switches, plus the `warnings` list. See `_expected.py:23-43`.
  - Values are per solver, or one `any` value when Gurobi and HiGHS recorded the same.
  - Of the 147 run values: 80 contain an error message, 73 are result tables, none are empty or INTERNAL. The result tables hold decision values, so they depend on solver tie-breaking.
- **How it was produced.**
  - Commit 97e7fd03cb recorded each value from flat's run (`DECIDB_RECORD_EXPECTED=1`, Gurobi then HiGHS), 35 minutes before flat was deleted in 19784f71b6.
  - At that commit `reference()` explicitly ran `DECIDB_EVALUATOR=flat`.
  - Before that, 1ec281897d claims verify showed "no difference from flat on any query of the suite".
- **Can it be regenerated?** Only by checking out 97e7fd03cb and rebuilding.
  - At the tip, `DECIDB_RECORD_EXPECTED=1` just re-records whatever the current tree prints. The `flat` argument is gone from the test code.
  - So "regenerate" is circular. The only guard is the docstring's "read the diff before committing".
  - The file records the recording method, but nothing in the repo makes it reproducible at the tip.
- **When behaviour legitimately changes.**
  - The test fails with a string diff.
  - The expected values are edited by re-recording from the new output. The reviewer's diff-reading is the only oracle.
  - This already happened once: 5 model files in dd356dd64e.
- **Is a flat-wrong answer frozen as correct?**
  - I found no instance by reading. Neither the D1/D2/D4 fix commits nor Q7 touched any stored file. That means no stored query exercised those defects. I did not review all 259 records, so I cannot rule it out.
  - The mechanism allows it: any flat output is accepted as correct, including alternate optima in the 73 result tables.
- **Correction to your note.** `test_error_precedence.py` does not read `_expected.py`. It defines a local `_expected()` helper at line 301. Only `test_tree_emitter.py` imports the store.

## Q4. A relative to the fork point

A adds about 15,300 test lines across 85 files and deletes about 530 lines of tests (`test_scope_determination.py`, `test_scope_check_plan.py`, `test_decidb_direct_rule_driver.cpp`). The deletions go with the code deleted in 2d3900eb9e.

A's tests are strongly oracle-based: `_anr_oracle.py` (1,243 lines) reads no SQL, and `test_anr_oracle_selfcheck.py` checks it five independent ways. The counts below are regex-based:

| File | Fns | Oracle-judged | Dump/EXPLAIN/profile pinned | Hand-stated answer or message |
|---|---|---|---|---|
| `test_anr_frames.py` | 110 | 49 | 2 | 59 |
| `test_error_binder.py` | 69 | 5 | 0 | 64 |
| `test_anr_nested.py` | 37 | 20 | 1 | 16 |
| `test_collated_keys.py` | 30 | 11 | 0 | 19 |
| `test_anr_composition.py` | 26 | 18 | 3 | 5 |
| `test_suite_log_compare.py` | 26 | 0 | 2 | 24 (tests the log comparer tool) |
| `test_fuzz_smoke.py` | 22 | 10 | 0 | 12 |
| `test_anr_inlining.py` | 17 | 15 | 1 | 1 |
| `test_anr_oracle_selfcheck.py` | 15 | 9 | 0 | 6 |
| `test_bound_side_tables.py` | 13 | 2 | 1 | 10 |
| `test_anr_variants.py` | 13 | 7 | 3 | 3 |
| `test_scope_variants.py` | 12 | 6 | 0 | 6 |
| `test_error_precedence.py` | 8 | 0 | 0 | 8 (ordered error pairs) |
| `test_anr_diagnostics.py` | 8 | 2 | 1 | 5 |
| `test_anr_reports.py` | 8 | 0 | 3 | 5 |
| `test_evaluator_verify.py` | 8 | 0 | 0 | 8 |
| `test_anr_row_order.py` | 7 | 1 | 2 | 4 |
| `test_scope_variant_paths.py` | 3 | 0 | 0 | 3 |
| `test_one_pass.py` | 1 | 0 | 1 | 0 |

- The classification above cannot separate "self-referential" from "hand-stated". A has no stored-output store. Its only dump pins are small model pins (`test_anr_nested.py` row pins, `test_auxiliary_bounds.py`).
- **Dead weight if A is kept alone:**
  - `test_evaluator_verify.py`: 8 fns, about 22 items. It tests the `DECIDB_EVALUATOR=verify` harness, which in A compares flat to flat ("Until the tree exists the second run is flat too"). Also a "the tree evaluator is not built yet" message test. It is the only test that needs the second evaluator.
  - `golden/capture.sh:17,80` pins `DECIDB_EVALUATOR=flat`.
  - `test_scope_reducer_by.py:252` (deleted in B) sets `DECIDB_EVALUATOR=flat`. `README.md:345` mentions it.
  - `test_suite_log_compare.py` (26 tests), `suite_log.py` and `capture_suite_log.sh` are tooling to compare two evaluators' logs. Harmless but built for the rewrite.
  - `test_anr_row_order.py` states it is "the baseline a rewrite of the evaluator is measured against". That is a feature, not dead weight.
- `test_one_pass.py` in A asserts `input_scans == [1]`. That is flat's counter, valid for A.

## Q5. Fuzzers (`test/decide/fuzz/`)

| Module | Generates | Independent check |
|---|---|---|
| `nests.py` | Reducers nested in reducers (when/per/by at every level), over decisions and over data. Over decisions it tries every assignment. The pinned kind reads each nest value. | `_anr_oracle.AnrModel`, a Python reading of the deck. Gurobi/HiGHS via `get_solver` for the solve; no helper or rewrite from DeciDB. |
| `frames.py` | `at(...)` and ranges with ties, NULLs and fallbacks | Same oracle |
| `bounds.py` | A frame on a clause bound over groups. Runs again with the bound precomputed as a column and compares. | Same oracle, plus `BoundModel` for D85 (two values under `=`). |
| `order_keys.py` | Collation, `COLLATE`, `default_collation`, NULL order, eight source shapes | Same oracle, plus a Python collation implementation |

Every case runs under both direct-solve modes and several table spellings. All four share one design: a Python re-implementation of the deck, written by the same authors. It is independent of DeciDB's code, but not of the authors' reading of the language.

Defect map (B's `fuzz/known.py` retains D3 only):

| Defect | Fix commit | Regression test | Fails if reverted? (from the commit message only) |
|---|---|---|---|
| D1 (MIN/MAX over a `by` key with a NULL) | d1d0ef49be | Seed 453 added to `test_fuzz_smoke.py` nests (oracle). Over-correction guard `test_scope_explain_diagnose.py:539`. | Yes: the message says seed 453 answered 1 before. Seed 3408 was not added. |
| D2 (`<>` with NULL bound drops a group) | 119b541c2c | `test_one_pass_execution.py` `NULL_NOT_EQUAL_CASES` (4 cases, hand-stated). `a-null-bound-excludes-nothing` is the D2 repro. | Yes: the message says the first case "fails without the mark". Seed 3657 was not added to the smoke seeds. |
| D4 (range over keyed decision with no instance, INTERNAL error) | 681ca9bd9b | Seed 37 added to frames smoke (oracle) | Yes: the message says seed 37 raised INTERNAL before. Only 1 of A's 15 failing seeds is kept. |
| D5 (HiGHS refusal, MIN/MAX of a pinned keyed decision) | dd356dd64e (+ ec7f347794 for seed 2235) | `test_fuzz_defects.py` `test_a_pinned_keyed_decision_bounds_a_max_on_highs` (HiGHS forced) and `test_a_crossed_box...` | Yes: the messages say the HiGHS refusal is reproduced without the fix. The failing seeds 104, 124 and 376 are not added. |
| D3 (AVG sums to 6.999999999999999, one ulp infeasible) | not fixed | none | n/a. `known.py:18` keeps it with an empty seed map. |

Also fixed with hand-worked tests in `test_fuzz_defects.py`: Q7-wrong-2, Q7-wrong-3, Q7-internal-abs, and Q7-ruling-equals (e153b7daf3, "the owner's ruling").

Oracle changes in B:
- `bounds.py` `BoundModel.generate` was rewritten so `=` is checked per clause by whether the MIN/MAX is written (`holds_extremum`). The `holds_orphan_group` step-aside was removed.
- `nests.py` dropped `holds_null`, `under_abs` and the `=` constant-only rule, so the generators now cover those shapes. `_anr_oracle.py` itself is unchanged in B.
- On the other hand, the `=` ruling is encoded in the fuzz oracle by the same commit that changed the engine. I cannot tell from the diff whether the ruling is independently documented.

## Q6. Coverage gaps by reading

**B's tree evaluator**
- **Real multi-threaded merge: none.**
  - Every merge test sets `SET threads = 1` and uses `DECIDB_DESCENT_SPLIT` (1 or 2 rows, 2048 in one test) to simulate threads.
  - The Finalize `MergeAll` that copies windows in parallel on `TaskExecutor` (36f12dd089) has no test with more than one thread.
  - The last commit, 7837e300be, fixed a crash "scanned on two threads or more" (130,000 rows, F nodes). The author found it outside the suite, and the added tests only simulate it via the split.
  - The suite's default thread count is `decidb_cli`'s default; its only `threads` setting is `SET threads=1` for model dumps (`decidb_cli.py:598`).
- **Large inputs.**
  - `test_error_time_limit.py` and `test_direct_*` have 20,000-1,100,000-row cases. I did not check whether they reach the tree's F-node paths.
  - No tree-specific scale or timing test exists. Performance claims (218 s vs 0.4 s) are guarded only by entry-count proxies such as `test_a_frame_with_no_else_keeps_one_entry_per_variant`.
- **Interrupts.** One test (`test_one_pass_execution.py:~233`): Ctrl-C 1.5 s into frame-window listing. It is timing-dependent. No interrupt test exists for the descent, the merge, or bottom-up evaluation.
- **INTERNAL error paths.** They are reached through test-only hooks in the production binary (`DECIDB_EVALUATOR_INJECT=g1` at `test_tree_bodies.py`, `DECIDB_TREE_INJECT` at `test_one_pass_execution.py`). Only `RequireRowReads` has a C++ unit test (3 sections).
- **Error ranking across threads.** `DecideErrorLedger::Rebase` is covered by one test with two errors. The existing `test_error_precedence.py:255` documents a thread race that stays unpinned.
- **Gurobi-only tests** skip on machines without a license. About 40 items in `test_tree_emitter.py` (POLYNOMIALS, DEFINITIONS, RAISED, F-node dumps) plus 4 in `test_tree_bodies.py`. HiGHS-only installs get no coverage of quadratic/product emission.
- **Stale names.** About 170 test names or docstrings still say "flat" (`..._as_flat_formulates_it`).
- **C++ `InlineReducerValueLinks` unit test.** The new `minmax_values` argument is never non-empty in `test_decidb_helper_inlining.cpp`.

**A's flat evaluator**
- D1, D2, D4 and D5 are open. The fuzzer's `known.py` counts them as known failures and does not fail the run, and no regression test fails today. D1 and D2 are wrong answers.
- The same thread race (`RACED`, `test_error_precedence.py`) is excluded.
- A has the same lack of multi-thread coverage; there are no row-order tests beyond reordering `VALUES`.
- The side-plan paths are covered by `test_bound_side_tables.py` (13 fns, 2 oracle-judged).

## Verdict

- **Which suite protects the product better: B's.**
  - B inherits A's oracle suite in full. It fixes four of A's five known defects with regression tests, and it adds oracle-judged tests where behaviour was fixed (product floor, ABS per group).
  - A's suite documents known wrong answers.
- **Against B's own evaluator, the protection is thinner than the file count suggests.**
  - Only 2 of B's 141 added test functions are judged against an independent oracle.
  - 53% compare against flat's recorded output, which cannot be regenerated at the tip.
  - 29% pin internal formats.
  - The real independent protection is A's oracle files (about 90) and the fuzz smoke seeds. The commit messages claim these pass on B at 10218+ cases, but I did not run them.
- **Brittleness to the planned change** (model writing moves into the tree nodes):
  - A: its oracle tests survive. Only `test_evaluator_verify.py` and the `DECIDB_EVALUATOR` pins are dead weight.
  - B: 44 added functions must be rewritten, and 38 more compare exact model text, which probably changes with emission order.
  - On B the cheap fix, re-recording the stored values, would hide a regression. So the stored-expectation tests are least useful exactly when the change they should guard happens.
- **Items to address regardless of branch** (unjustified expectation changes): EXPLAIN losing the ABS/L0 generated rows; `test_one_pass.py` asserting only `> 0`; the vacuous memory-limit loop; the `_old_spellings.py` exemption; and the missing real multi-thread test.

## What I could not determine

- Whether any test passes or fails. I did not run anything.
- Whether each regression test fails when its fix is reverted; I rely on commit messages.
- Whether any of the 259 stored records contains a flat bug. I sampled two and read the 5 re-recorded ones.
- Exact parametrized item counts (they are derived from dict sizes) and the exact oracle/hand-stated split in A's files (regex-based).
- Whether the large-input tests in `test_error_time_limit.py` and `test_direct_*` reach the tree's parallel merge.
- Whether the `Q7-ruling-equals` and "owner's ruling" semantics are documented outside the commit message.
- Why the golden's `ub=40` to `ub=25` change was recorded without a commit body, and whether the EXPLAIN row removal is documented in `context/docs`. My search did not find it.

Key files, all under `/Users/hatim/Code/decidb/.claude/worktrees/sami-opa/test/decide/tests/` unless noted:
- `_expected.py`
- `expected/`
- `test_tree_emitter.py`
- `test_tree_bodies.py`
- `test_one_pass_tree.py`
- `test_one_pass_literal.py`
- `test_one_pass_execution.py`
- `test_descent_merge.py`
- `test_fuzz_defects.py`
- `test_explain.py`
- `test_scope_explain_diagnose.py`
- `test_one_pass.py`
- `_old_spellings.py`
- `/Users/hatim/Code/decidb/.claude/worktrees/sami-opa/test/decide/fuzz/known.py`
- `/Users/hatim/Code/decidb/.claude/worktrees/sami-anr/test/decide/tests/test_evaluator_verify.py`
