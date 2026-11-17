# Testing — open work

Open coverage gaps, one section each. Coverage that exists is in [coverage.md](coverage.md) and
[scope_suite.md](scope_suite.md). How to work and the checks:
[`../direct_solve/framework/todo.md`](../direct_solve/framework/todo.md) § How to work any task in these todo files.

## TEST-3 — Cut the dump-format tests

- **Problem.** 31 tests pin the evaluator's internal dump text rather than answers: `test_tree_bodies.py` (15),
  `test_one_pass_tree.py` (11) and `test_one_pass_literal.py` (5) (`context/opa_rewrite/handoff.md` § A9). Any change
  to the tree's internals breaks them without a wrong answer.
- **Change.** By the owner's ruling (handoff § A6, item 11), delete them except the two that check the deck's pictures
  (p. 81's tree, p. 91's containers). The owner asked for a coverage check: before deleting, `make decide-coverage`
  shows which branches each one reaches, and a test that is a branch's only test stays or gives way to an answer-level
  test of that branch.
- **Ruling (owner, 2026-11-17).** One coverage run, before the deletion: deleting tests changes no code, so the arms
  lost are those the deleted tests reach and no kept test does, read from the run's per-test profiles
  (`build/coverage/run/report.json`).
- **Done when.** No arm is reached only by the deleted tests;
  `./test/decide/run_tests.sh -n 2 -k "tree_bodies or one_pass_tree or one_pass_literal"` passes what remains.

## TEST-4 — Tests that check less than their names say

- **Problem.**
  - `test_one_pass.py::test_the_evaluation_reads_the_input_once` claims the evaluation never scans a buffer; it checks
    only that one descent ran with some rows.
  - In `test_direct_r1_continuous_knapsack.py::test_r1_copy_leaves_out_the_stored_strings_it_reads_back_by_row_id`,
    the solver-path loop over two memory limits asserts the same plan twice.
  - `test_error_binder.py::TestRefusalsInEverySpelling::test_a_string_that_reads_as_a_tag_is_left_as_written` guards
    against fragment tags, which no code produces.
  - `test_scope_reducer_by.py` has no test that a `by` key is computed once per row (handoff B9, item 5).
  - `test_anr_inlining.py` imports `At` and `dump_bounds` and uses neither.
  - `test_scope_bare_clauses.py::test_a_negative_floor_does_not_widen_a_bool`: in the default mode a direct rule (A1)
    answers its `BETWEEN` spelling, so the solver-path floor it names runs only under `DECIDB_TEST_DIRECT_SOLVE=off`.
- **Change.** The one-pass test asserts the descent read exactly the input's rows, without the buffer claim. The
  knapsack loop keeps one limit. The tag test goes unless coverage shows a branch only it reaches. The `by` key test
  reads a profile counter; if none counts key evaluations, bring the owner the choice. The unused imports go. The
  negative-floor test runs its `BETWEEN` spelling with `decide_direct_solve = 'off'`.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "one_pass or r1_copy or reads_as_a_tag or scope_reducer_by or
  anr_inlining or negative_floor_does_not_widen"` passes.

## TEST-6 — One suite run on a debug build

- **Problem.** No debug build ran the merged branch, so its debug-only assertions never ran
  (`context/opa_rewrite/merge_notes.md`, trap 6).
- **Change.** By the owner's ruling (handoff § A6, item 11): build debug once, after every other task in the queue,
  and run `make decide-test` against it in the background. It is a long CPU-bound run, so confirm the start with the
  owner.
- **Needs.** Every other task of `context/docs/todo.md`.
- **Done when.** The run passes or each failure is triaged, and the result is in `merge_notes.md` § 4.1.

## TEST-7 — What test_tree_emitter.py's deleted tests checked

- **Problem.** Seven tests of `test_tree_emitter.py` read no stored output and left with the file (R3):
  `test_a_boolean_product_is_a_function_node`, `test_an_l0_norm_is_a_function_node`,
  `test_a_nonlinear_reducer_body_is_a_function_node`, `test_abs_reads_a_nonlinear_reducer_function`,
  `test_a_null_in_a_later_window_marks_its_own_entry`, `test_a_covered_query_prints_each_dump_it_is_asked_for` and
  `test_a_helper_over_every_row_is_one_column`. None exists on the branch. The review finds no test of an L0 indicator
  as an F node, `function=definition`, `DECIDB_DUMP_VALUES`, the NULL entry of a later window, or the 6,001-entry helper
  column. `git show f56845f560:test/decide/tests/test_tree_emitter.py` holds them.
- **Change.** With TEST-3 and EXEC-4, decide which of these properties are owed a test and port those: an oracle test
  where an answer depends on the property, a `decide_debug` test where only a dump shows it (EXEC-4's `values` test is
  one). A property only a dump shows is the owner's call, since TEST-3 deletes dump-format tests.
- **Needs.** TEST-3, EXEC-4.
- **Done when.** Each property has a test or an owner ruling; `./test/decide/run_tests.sh -n 2 -k "<the ported tests>"`
  passes.

## TEST-8 — Two cases of the too-large-sum test run in no mode

- **Problem.** `test_invalid_value_message.py::test_a_sum_past_a_double_names_its_clause` takes mainline's mode list
  `["off"]`, so `group_sums_of_a_nested_objective`'s `auto` cases (Gurobi and HiGHS) do not run, and
  `context/opa_rewrite/merge_notes.md` § 10 has no row for them. Its row M9 counts 6 HiGHS halves removed; the review
  counts 7.
- **Change.** Restore the `auto` cases, or add the § 10 row giving the reason; correct M9's count from the commit that
  removed them.
- **Done when.** The cases run, or the row states why not, and M9's count matches the commit;
  `./test/decide/run_tests.sh -n 2 -k sum_past_a_double` passes.
