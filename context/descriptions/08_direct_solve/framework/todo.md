# Shared engine — open work

The engine is everything that is not one problem class: IR, rule driver, plan factory functions, result adapter, policy.
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

## H-20 — A reducer's own `WHEN` and its empty group in the rewrite

- **Problem.** `SUM(x) <= SUM(cap) WHEN active` scopes the right-hand reducer apart from the clause. The rewrite leaves these
  on the C++ fold.
- **Change.** The rewrite carries the reducer's `WHEN` as the window aggregate's `FILTER`. A group whose filter keeps no
  row raises the empty-aggregate error, as the fold does.
- **Done when.** Differential cases for a reducer `WHEN` with and without `PER`, a group the filter empties, and a
  `WHEN` on both the clause and the reducer pass under `off` and `require`. The C++ fold holds only the relation-qualified
  reducers.

## H-21 — Split `direct_plan_factory` by job

- **Problem.** `direct_plan_factory.{hpp,cpp}` holds four jobs: plan fragments (breaker, empty-aggregate guard, count
  windows), runtime checks and their messages (`DirectValid*Predicate`, `DirectNullMessageOf`, the infeasible and
  unbounded errors), the per-row bound reader (`DirectReadBounds`, `DirectClauseBounds`, `DirectRowBox`, which read the
  IR and are not plan fragments), and source analysis that only the driver calls (`DirectCanSkipSourceOutput`).
  `DirectProveValue` is an admission predicate living outside `direct_expression`. The two infinity constants are
  defined three times. A third rule would add to the pile.
- **Change.** Three files: plan fragments, runtime checks, bound reader. `DirectCanSkipSourceOutput` goes beside the
  driver, `DirectProveValue` into `direct_expression`, and the infinity constants are defined once. No behavior change.
  `architecture/rules.md` § Adding a rule step 4 and `framework/done.md` name the new homes.
- **Done when.** The five checks pass with no edit to a test file.

## H-22 — The bound reader holds what R1 needs from A1

- **Needs.** H-21.
- **Problem.** R1 needs two pieces A1 keeps to itself: the shape check that a per-row bound reads one decision
  (`ReadsOneDecision`) and the empty-box test in A1's answer stage (`ComputeAnswers`).
- **Change.** Both move into the bound reader; A1 calls them there.
- **Done when.** The A1 suites pass unchanged and A1 has no copy of either.

## H-23 — One reader for a linear objective

- **Needs.** H-21.
- **Problem.** S1 (`ProveObjective`) and A1 (the linear branch of its `ProveObjective`) each turn `SUM(c*x)` parts into
  sign, coefficient and scale, into two structs with the same fields (S1's `Condition::ObjectivePart`, A1's `Part`).
  R1 would write a third.
- **Change.** A shared `DirectReadLinearObjective` beside `DirectReadBounds`, the same shared-reader pattern. S1 calls
  it; A1 calls it for linear terms and keeps squares as its own.
- **Done when.** The S1 and A1 suites pass unchanged.

## H-24 — Shared helpers for "true, or raise"

- **Needs.** H-21.
- **Problem.** Plan code raises errors through `CASE ok THEN true ELSE error(...)`, written by hand in six places (S1's
  `FeasiblePredicate`, A1's `ValidAnswers`, `ValidateBounds`, `DirectGuardEmptyAggregate`, `DirectValidNumberPredicate`,
  `DirectValidBoundPredicate`). A list of such checks is joined in five places, two ways: a `CASE` chain (A1's
  `ComputeBounds` and `ValidAnswers`, `ValidateBounds`) and an `AND` chain (twice in S1's `FeasiblePredicate`).
- **Change.** `DirectRaiseUnless(ok, error)`, `DirectRaiseIf(bad, error)` and `DirectAllHold(checks)` among the runtime
  checks, one way of joining; every caller uses them. Which error a query reports among several stays unspecified
  (`architecture/rules.md`).
- **Done when.** The five checks pass with no edit to a test file.

## H-25 — `ExpressionBuilder` is the one way to build an expression

- **Needs.** H-24.
- **Problem.** Direct-solve code builds bound expressions three ways: by hand with `make_uniq<Bound...>` (S1 51 times,
  its helpers 17, A1 16, the plan factory 27), through `DirectColumn` and `DirectConstantBool`, and through
  `ExpressionBuilder` (A1 46 times, S1 never). S1's `AnyCondition` copies `ExpressionBuilder::Any`. A new rule copies
  whichever style it reads first.
- **Change.** Give `ExpressionBuilder` the operations S1 and the plan factory need (`And`, `IsNull`, `Cast`, and any
  other the conversion finds), and convert the shared files and A1. S1 is S1-18. `architecture/rules.md` names the
  builder as the way to build an expression.
- **Done when.** The five checks pass with no edit to a test file. Outside S1, a hand-built expression remains only
  where the builder has no operation for it, with a comment saying so.

## H-26 — The driver turns away an IR it cannot read

- **Problem.** Every rule opens `CheckCondition` with the same checks of the IR's status: decisions, source,
  constraints, each constraint, the objective, a finite objective offset and a known sense (S1 and A1, about 25 lines
  each). R1 would copy them.
- **Change.** The rule driver checks once and offers the IR to no rule when it fails. Each rule drops its copy; a rule
  keeps only the checks that are about its own class.
- **Done when.** Neither rule repeats the checks and the five checks pass.

## H-27 — Test literals and rule headers

- **Problem.** The S1 and A1 oracle suites each define `_literal`, differently (S1 wraps in `CAST`, A1 writes `::DOUBLE`
  and spells infinities). Each rule has a header holding one factory declaration, so every class adds a one-line
  header.
- **Change.** One `sql_literal` in `_direct_oracle_common.py` that both suites call; one `direct_rules.hpp` declaring
  every rule's factory function. `architecture/rules.md` § Adding a rule step 1 follows. Test function bodies stay as
  they are, so the oracle cache still hits.
- **Done when.** The five checks pass with no oracle re-run.

## H-29 — Prune the direct-solve tests

- **Needs.** S1-17 and H-26, so the tests are weighed against the final code and the final coverage.
- **Problem.** The direct-solve tests are about half of the suite (1,746 of 3,445 tests; S1's oracle file alone has
  1,141, from parametrized grids) and the matrix runs each test four times. Some tests may repeat each other across files,
  pin how a rule is built instead of what the user gets, or sit in a grid whose extra cases reach no new branch.
- **Change.** Rebuild the catalogue of the direct-solve tests by category (what each category checks, against what,
  and which behavior it pins; the first catalogue had 19 categories and the owner judged each a meaningful check).
  With the per-test branch coverage (`run_coverage.sh`, `framework/done.md`), list per category the tests whose removal leaves the set of covered branches
  unchanged. Tests that pin a behavior the owner finds arbitrary go to the owner as a question about the system, not the
  test. The owner picks what to cut; each cut is a task of its own, and the coverage run after it must show no branch lost.
- **Done when.** The owner has the report and has said which cuts to make.

## H-31 — Test the decline branches on the IR

- **Needs.** H-26.
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
