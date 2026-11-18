# Testing — open work

Open coverage gaps, one section each. Coverage that exists is in [coverage.md](coverage.md) and
[scope_suite.md](scope_suite.md). How to work and the checks:
[`../direct_solve/framework/todo.md`](../direct_solve/framework/todo.md) § How to work any task in these todo files.

## TEST-6 — One suite run on a debug build

- **Problem.** No debug build ran the merged branch, so its debug-only assertions never ran.
- **Change.** By the owner's ruling (2026-11-17): build debug once, after every other task of Phase 0,
  and run `make decide-test` against it in the background. It is a long CPU-bound run, so confirm the start with the
  owner.
- **Needs.** Every other task of Phase 0 in `context/docs/todo.md`.
- **Done when.** The run passes or each failure is triaged, and the result is in
  `context/opa_rewrite/README.md` § The gate for every change.

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

## TEST-13 — Four merge choices waiting on the owner

- **Decision needed.** The merge left four choices marked "owner to confirm":
  1. Two tests in `test_one_pass_tree.py` read a node's reads from the dump's `reads=` field.
  2. A product or square over a nested reducer under a bound that reads the rows is refused by name
     (`context/opa_rewrite/repro/refusals.sql`, last query).
  3. Four decline shapes in `test_decidb_direct_decline.cpp` are written as an IR edit.
  4. `test_decidb_helper_inlining.cpp` reads product groups in two cases where OPA had four.
- **Options.** Per item: keep as is, or change (1: assert answers, not the dump; 2: solve it; 3: write each as a
  query; 4: restore the two cases).
- **Recommendation.** 1 and 3 change (a test that reads a dump or edits the IR breaks when Phase 5 replaces both); 2
  stays refused until Phase 6 gives products a node rule; 4 restores the two cases if they reach a branch the others
  do not.
- **Ruling (owner, 2026-11-18).** As recommended: 1 asserts answers, not the dump; 3 writes each decline as a
  query; 2 stays refused until Phase 6; 4 restores the two cases only if each reaches a branch the others do not.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "one_pass_tree"` and
  `build/release/test/unittest "[decidb]"` pass.

## TEST-15 — One helper asserts an error's kind and location

- **Problem.** About 690 of 2,533 test functions compare message text (108 of 177 test files), so each rewording of
  Phase 1 breaks dozens of them, and tasks that run in parallel edit the same assertions.
- **Change.** By the owner's ruling (2026-11-18, `context/opa_rewrite/decisions.md` R16): one helper beside
  `assert_error` (`test/decide/decidb_cli.py`) asserts an error's kind and the location it quotes. It lands before
  any message changes; a Phase 1 task that rewords a message converts the tests it breaks to the helper.
- **Done when.** The helper has a test that fails on a wrong kind and on a wrong location;
  `./test/decide/run_tests.sh -n 2 -k "error"` passes.

## TEST-14 — A data-weighted keyed clause under the infeasible diagnosis

- **Problem.** A keyed decision summed with a data weight (`SUM(keepS * cost)`) renders symbolically in the infeasible
  diagnosis, but no test reaches it; the keyed-fold label tests in `test_query_diagnostics_relation.py` cover only
  uniform coefficients.
- **Change.** One test that applies the reported edit with `_apply_reported_fix`.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "query_diagnostics_relation"` passes.
