# Testing — open work

Open coverage gaps, one section each. Coverage that exists is in [coverage.md](coverage.md) and
[scope_suite.md](scope_suite.md). How to work and the checks:
[`../direct_solve/framework/todo.md`](../direct_solve/framework/todo.md) § How to work any task in these todo files.

## TEST-11 — Error tests on the solver path assert kind and location

- **Problem.** About 690 of 2,533 test functions compare message text, so a rewording breaks dozens of them.
- **Change.** By the owner's ruling (2026-11-18, `context/opa_rewrite/decisions.md` R16): the solver-path test files
  that still compare message text use TEST-15's helper. The full text of each message lives in `language/errors.md`
  only.
- **Needs.** TEST-15; EXEC-15, EXEC-16, PARSE-2, PARSE-3, BIND-2, OPT-19, OPT-20 (each converts the tests its
  rewording breaks; this task converts the rest).
- **Done when.** The converted files pass: `./test/decide/run_tests.sh -n 2 -k "error"`.

## TEST-12 — Error tests on the direct path assert kind and location

- **Problem.** `test_direct_user_facing.py` (1,699 lines) and the direct decline tests compare whole messages.
- **Change.** As TEST-11, with its helper, for the direct-path files.
- **Needs.** TEST-11, FW-4.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct"` passes.

## TEST-15 — One helper asserts an error's kind and location

- **Problem.** About 690 of 2,533 test functions compare message text (108 of 177 test files), so each rewording of
  Phase 1 breaks dozens of them, and tasks that run in parallel edit the same assertions.
- **Change.** By the owner's ruling (2026-11-18, `context/opa_rewrite/decisions.md` R16): one helper beside
  `assert_error` (`test/decide/decidb_cli.py`) asserts an error's kind and the location it quotes. It lands before
  any message changes; a Phase 1 task that rewords a message converts the tests it breaks to the helper.
- **Ruling (owner, 2026-11-18).** An error's kind is the DuckDB error class on the message's first line (`Parser
  Error`, `Binder Error`, `Invalid Input Error`, ...); no list of kinds is added to `language/errors.md` or the code.
  Its location is what the message quotes from the query: the `LINE n:` caret line, or the quoted SQL fragment where
  there is no caret. A test asserts the kind, and the location where the message has one, and no other message text;
  the wording lives in `language/errors.md` only. Two errors of one class at one place look the same to the helper; a
  test that must tell them apart matches one more word.
- **Done when.** The helper has a test that fails on a wrong kind and on a wrong location;
  `./test/decide/run_tests.sh -n 2 -k "error"` passes.
