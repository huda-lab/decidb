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
- **Change.** The one-pass test asserts the descent read exactly the input's rows, without the buffer claim. The
  knapsack loop keeps one limit. The tag test goes unless coverage shows a branch only it reaches. The `by` key test
  reads a profile counter; if none counts key evaluations, bring the owner the choice. The unused imports go.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "one_pass or r1_copy or reads_as_a_tag or scope_reducer_by or
  anr_inlining"` passes.

## TEST-6 — One suite run on a debug build

- **Problem.** No debug build ran the merged branch, so its debug-only assertions never ran
  (`context/opa_rewrite/merge_notes.md`, trap 6).
- **Change.** By the owner's ruling (handoff § A6, item 11): build debug once, after every other task in the queue,
  and run `make decide-test` against it in the background. It is a long CPU-bound run, so confirm the start with the
  owner.
- **Needs.** Every other task of `context/docs/todo.md`.
- **Done when.** The run passes or each failure is triaged, and the result is in `merge_notes.md` § 4.1.
