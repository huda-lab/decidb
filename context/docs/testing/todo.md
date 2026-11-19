# Testing — open work

Open coverage gaps, one section each. Coverage that exists is in [coverage.md](coverage.md) and
[scope_suite.md](scope_suite.md). How to work and the checks:
[`../direct_solve/framework/todo.md`](../direct_solve/framework/todo.md) § How to work any task in these todo files.

## TEST-11 — Error tests on the solver path assert kind and location

- **Problem.** About 690 of 2,533 test functions compare message text, so a rewording breaks dozens of them.
- **Change.** By the owner's ruling (2026-11-18, `context/opa_rewrite/decisions.md` R16): the solver-path test files
  that still compare message text use `assert_error_at` (`testing/oracle.md` §3.6). The full text of each message lives in `language/errors.md`
  only.
- **Converted so far.** The messages tasks of Phase 1 moved the tests their rewording broke to `assert_error_at`:
  `grep -rc assert_error_at test/decide/tests` counts 333 lines (328 calls, in 66 files). 19 files still call
  `assert_error(` with `match=` (41 calls): `test_bilinear.py`, `test_canonicalize_cast.py`, `test_canonicalize_scale.py`,
  `test_clause_order.py`, `test_coefficient_window.py`, `test_cons_perrow.py`, `test_error_binder.py`,
  `test_error_infeasible.py`, `test_error_parser.py`, `test_error_unsupported_operator.py`, `test_infinite_bounds.py`,
  `test_large_number_warning.py`, `test_mip_gap.py`, `test_nested_decide.py`, `test_norm.py`, `test_per_qualified.py`,
  `test_quadratic_constraints.py`, `test_query_diagnostics_relation.py`, `test_reducer_filter.py`. Other files compare
  message text without the helper (`in result.stderr`).
- **Needs.** This task converts the files above and the rest.
- **Done when.** The converted files pass: `./test/decide/run_tests.sh -n 2 -k "error"`.

## TEST-12 — Error tests on the direct path assert kind and location

- **Problem.** `test_direct_user_facing.py` (1,699 lines) and the direct decline tests compare whole messages.
- **Change.** As TEST-11, with its helper, for the direct-path files.
- **Needs.** TEST-11.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct"` passes.

## TEST-16 — A frame reducer reads no row in no class

- **Problem.** A row in no class of a `per` key is read by nothing below the `per` (D81, the Decision 2 ruling of
  `pipeline/08_execution/done.md`). No test has an anchor in no class reach a frame's reducer, so a frame's operand could
  read that row and raise.
- **Change.** A test whose anchor has a NULL `per` column and reaches a frame reducer over a row that would raise, and
  one whose objective sum, objective max and constraint agree on the same rows. The test fails if the branch that
  drops the row is removed.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "class_null_key or frame"` passes.

## TEST-17 — Branches of the new messages no test reaches

- **Problem.** Some arms raise a message no test reaches: a term beside a `MIN` or `MAX` that the rewrite does not read
  (`'q' beside MIN or MAX is not supported`; 26 shapes were tried and none reaches it, and none is proved unreachable),
  the internal error of `decide_linear_form.cpp` that expects a rewritten objective aggregate, the solver statuses
  `infeasible or unbounded` and the iteration limit, the Gurobi model-load failures (`Failed to add ... to Gurobi`,
  `GRBaddqconstr`) and the Gurobi environment failure.
- **Change.** A test for each that a query can reach; the others are listed in `testing/coverage.md` as uncovered.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "error or unbounded or infeasible"` passes.
