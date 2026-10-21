# Shared engine — open work

The engine is everything that is not one problem class: IR, rule driver, plan fragments, runtime checks, bound reader,
result adapter, policy.
What exists today is in `done.md`.

## How to work any task in these todo files

- Each task stands alone. Read its **Problem**, make the change and run the checks. When it ships, its result moves to
  the owning `done.md` and the task is deleted from its `todo.md`.
- The baseline to compare against is the stamp at the top of `done.md`. Any other failure after your change is yours.
- Checks that every code task runs (from the repository root): `make decide-test`,
  `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`, `DECIDB_VERIFY_SERIALIZER=1 make decide-test`,
  `DECIDB_FORCE_SOLVER=highs make decide-test` (one failure that needs Gurobi is expected: `test_norm_combined_l1_l2_objective`),
  and `build/release/test/unittest "[decidb]"`. Rebuild first with `make release`. `test/decide/run_direct_matrix.sh` runs all
  five and prints one line each (about 3 minutes). Doc-only tasks need no checks.
- Do not loosen or delete a test to make it pass. If a test fails for a reason the task did not predict, stop and report it.
- Tasks marked **Decision needed** have two or more reasonable answers. Ask the owner before coding.
- The oracle tests cache solver results under the test's node id (`results/oracle_cache.json`). Do not rename a test
  function or move it to another file unless the task says so; a miss re-runs Gurobi, which needs a licence.
- The Bash tool is zsh. In `grep`, put `--include` flags before `-e PATTERN`; after a `--` they are read as file names.

## H-02 — Front end for the new language

- **Needs.** A stable language branch.
- **Problem.** The rules read an IR built from the current syntax only (`direct_ir.cpp`;
  `../architecture/ir.md`).
- **Change.** A second front end builds the same IR from the new syntax. Extend the IR when semantics change; do not
  teach a rule parser spelling.
- **Done when.** The IR contract and the direct/solver tests run on both syntaxes where comparable.

## H-25 — `ExpressionBuilder` is the one way to build an expression

- **Problem.** Direct-solve code builds bound expressions three ways: by hand with `make_uniq<Bound...>` (S1 51 times,
  its helpers 17, A1 16, the shared plan files 27), through `DirectColumn` and `DirectConstantBool`, and through
  `ExpressionBuilder` (A1 46 times, S1 never). S1's `AnyCondition` copies `ExpressionBuilder::Any`. A new rule copies
  whichever style it reads first.
- **Change.** Give `ExpressionBuilder` the operations S1 and the shared plan files need (`And`, `IsNull`, `Cast`, and any
  other the conversion finds), and convert the shared files and A1. S1 is S1-18. `architecture/rules.md` names the
  builder as the way to build an expression.
- **Done when.** The five checks pass with no edit to a test file. Outside S1, a hand-built expression remains only
  where the builder has no operation for it, with a comment saying so.

## H-29 — Prune the direct-solve tests

- **Problem.** The direct-solve tests are 1,369 of the suite's 3,090 and the matrix runs each four times. Their per-test
  branch coverage (`framework/done.md`) shows 67 tests reaching every arm that any test reaches; S1's file has 786
  tests and 16 were in that cover before S1-17 removed the offset tests. `S1_OFFSET_CASES` and the offset-named
  helpers beside it now hold only negations (`SUM(-x)`); their names predate S1-17.
- **Mutation table.** 45 one-line changes to value-handling code (a comparison, a constant, an operator) were each
  run against the six `test_direct_*.py` files on the October 2026 code. 38 were caught and 7 survived. 14 tests catch
  all 38, and 577 of the 1,746 tests caught none. The tests that catch nothing count only where the changes could reach
  them: in the S1 oracle grid 62 of 277, the S1 reducer grid 36 of 242, the A1 oracle grid 16 of 135. The near misses
  (58 of 61) and `test_direct_source_values.py` (92 of 108) check declines and errors, which no change touched, so
  their counts carry no signal. Keep the sole catchers: `impossible_upper_bound_per_is_infeasible` (S1 error cases) and
  `optimal_when_the_open_row_is_outside_the_objective` (A1 oracle grid), and every fuzz case (all 24 caught a change).
  Survivors that may be untested boundaries: a strict count bound at exactly 2^53 in S1, an A1 row with a zero
  coefficient and no lower bound, a Boolean lower bound below 0 in `DirectReadBounds`, and the rounding steps of the
  integer search in `direct_bound_reader.cpp`. The per-change and per-test data, the change list and the runner are in
  `framework/h35/`, which git ignores; its change list names `direct_plan_factory.cpp`, whose code now lives in `direct_plan_fragments.cpp`,
  `direct_runtime_checks.cpp` and `direct_bound_reader.cpp`, and later changes shift line numbers, so re-map the change
  list before a re-run.
- **Change.** Rebuild a catalogue of the direct-solve tests by category: what each checks, against what, and which
  behavior it pins. With the coverage and H-35's table, list per category the tests that add no branch and catch no
  mutation. The owner picks what to cut; each cut is a task of its own, and the coverage run after it must show no
  branch lost.
- **Done when.** The owner has the report and has said which cuts to make.

## H-31 — Test the decline branches on the IR

- **Problem.** Each "not exactly this shape" branch in `CheckCondition` is tested by a full SQL query on up to four
  paths, though a rule's input is a plain `DirectIR` struct. The branches cost most of the suite.
- **Change.** A C++ helper that builds a small `DirectIR` and calls a rule's `CheckCondition`, with one case per decline
  branch for S1, A1 and the shared driver check. The SQL and oracle tests stay for what they prove, which is that
  answers are right.
- **Done when.** Every decline branch of S1 and A1 has an IR-level case and the per-test coverage shows the same branches
  covered. Which SQL tests it makes unneeded is H-29's work.

## H-32 — Coverage and complexity in the daily checks

- **Problem.** The `CLAUDE.md` rule that tests follow decision points has no check behind it, and coverage runs only
  when someone sets it up. Running a subset of tests by hand also deletes the other tests' oracle-cache entries unless
  `-m` is passed (`conftest.py` prunes after a run with no `-k` or `-m`).
- **Change.** A `make decide-coverage` target around `run_coverage.sh`, and `/orchestrate`'s exit gate reports the
  complexity change of the functions a task touches and the tests that reach each branch it adds.
- **Done when.** The target runs from the repository root, and the orchestrate skill and `.claude/CLAUDE.md` name it.

## H-34 — Branches no test reaches

- **Problem.** 364 of 2,008 branch arms in the direct-solve files are reached by no test (`s1_uniform_matroid_rule.cpp` 80,
  `direct_late_materialization.cpp` 53, `direct_ir.cpp` 50, `a1_separable_box_rule.cpp` 38, `direct_expression.cpp` 35,
  `decide_rewrite_rhs_window.cpp` 32, `direct_result_adapter.cpp` 28, the three files split from the plan factory 23,
  `direct_rule_driver.cpp` 17, `s1_uniform_matroid_helpers.cpp` 8). Each is an untested guard or dead code. Known arms no
  SQL reaches: `DirectIRIsReadable` on a not-proved decisions or source status and on an unknown sense, and the
  right-hand-side rewrite's clause-`WHEN` path under a reducer's own `WHEN` (the binder rejects both `WHEN`s together).
- **Change.** Re-run the coverage and list the unreached arms with file and line. For each, name a query that reaches it
  and add that test, or delete the code when no query can.
- **Done when.** Every listed arm is reached by a test or gone, and the five checks pass.
