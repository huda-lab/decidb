# Testing — open work

Open coverage gaps, one section each. Coverage that exists is in [coverage.md](coverage.md) and
[scope_suite.md](scope_suite.md). How to work and the checks:
[`../direct_solve/framework/todo.md`](../direct_solve/framework/todo.md) § How to work any task in these todo files.

## TEST-11 — Error tests on the solver path assert kind and location

- **Problem.** About 690 of 2,533 test functions compare message text, so a rewording breaks dozens of them.
- **Change.** By the owner's ruling (2026-11-18, `context/opa_rewrite/decisions.md` R16): the solver-path test files
  that still compare message text use `assert_error_at` (`testing/oracle.md` §3.6). The full text of each message lives in `language/errors.md`
  only.
- **Needs.** EXEC-15, EXEC-16, PARSE-2, PARSE-3, BIND-2, OPT-19, OPT-20 (each converts the tests its
  rewording breaks; this task converts the rest).
- **Done when.** The converted files pass: `./test/decide/run_tests.sh -n 2 -k "error"`.

## TEST-12 — Error tests on the direct path assert kind and location

- **Problem.** `test_direct_user_facing.py` (1,699 lines) and the direct decline tests compare whole messages.
- **Change.** As TEST-11, with its helper, for the direct-path files.
- **Needs.** TEST-11, FW-4.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct"` passes.
