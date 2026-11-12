# Shared engine — open work

The engine is everything that is not one problem class: IR, rule driver, plan fragments, runtime checks, bound reader,
result adapter, policy.
What exists today is in `done.md`. Direct solve declines each language construct it cannot yet model; the
solver path answers those queries, so none of them is wrong, only slower.

## How to work any task in these todo files

- Each task stands alone. Read its **Problem**, make the change and run the checks. When it ships, its result moves to
  the owning `done.md` and the task is deleted from its `todo.md`.
- The baseline to compare against is the stamp at the top of `done.md`. Any other failure after your change is yours.
- A task runs only the focused tests its **Done when** names, after `make release`. The full gate runs once where
  the work is combined or committed, not per task: `make decide-test`, `DECIDB_TEST_DIRECT_SOLVE=off make
  decide-test`, `DECIDB_VERIFY_SERIALIZER=1 make decide-test`, `DECIDB_FORCE_SOLVER=highs make decide-test`, and
  `build/release/test/unittest "[decidb]"`; `test/decide/run_direct_matrix.sh` runs all five and prints one line each
  (about 3 minutes). Doc-only tasks need no checks.
- Do not loosen or delete a test to make it pass. If a test fails for a reason the task did not predict, stop and report
  it.
- Tasks marked **Decision needed** have two or more reasonable answers. Ask the owner before coding.
- The oracle tests cache solver results under the test's node id (`results/oracle_cache.json`). Do not rename a test
  function or move it to another file unless the task says so; a miss re-runs Gurobi, which needs a licence.
- The Bash tool is zsh. In `grep`, put `--include` flags before `-e PATTERN`; after a `--` they are read as file names.
- Stop and ask also when a golden changes that the task did not name, or a user-facing message needs wording the
  doc does not give. A task that turns out to need the owner records its question as **Decision needed**.
- A behavior change is judged by an oracle or the solver path, on both backends. A structural change gets a
  `decidb-architect` review.
- Never without the owner: large-tier benchmarks or other long CPU-bound runs, regenerating goldens before reading the
  diff, pushing, new branches or worktrees, `git stash`.
- **Removing bloat (the BLOAT tasks, in every area).** Bloat is code or test that removing changes no answer, error
  text, warning or plan cost: a branch no query reaches and the tests that reach only it; a helper or type that
  duplicates another (the second copy goes, and a helper two callers share moves to shared code); a wrapper or
  parameter that only forwards; a grid case that reaches no branch another case reaches (`.claude/CLAUDE.md`
  § Testing); a comment that restates the code. Behavior stays as it is: a removal that changes an answer, an error
  text or a golden is not bloat, so stop and report it. Each removal names its evidence (no caller, a mutation that
  fails no test, the case that already reaches the branch). Record the lines and the `lizard` figures of the area
  before and after in the report.
- **Small gains.** A change that adds an analysis, a mechanism or a special case for a gain inside the benchmark's
  noise (±15% of CPU on a loaded machine) or a few milliseconds of latency is not done, and landed work of that kind is
  removed; a change that simplifies is done even when it gains nothing. A ratio target is an aim, not a reason to add
  machinery. Not done on this rule: a smallest-magnitude analysis to drop the below-normal check of a value of an exact
  type (a2_abs_global, 0.6 ms); R1's per-row answer from window frames (a second plan shape for several decisions, and
  prefix sums the segment tree does not keep monotone); proving R1's group checks at plan time. **Ruling (owner,
  2026-11-09).** Not done either: filling S3's whole-table windows from every thread. A constant partition sorts every
  row and takes 1.8 times the hand plan's CPU for `s3_global`; a shared row stage with an aggregate joined back is a
  second plan shape; DuckDB's one-thread `OVER ()` keeps table order for other queries. **Ruling (owner, 2026-11-11).**

## FRM-12 — A late error in a later group in every fixture that groups rows

- **Problem.** `test_every_input_row_is_read_before_any_row_is_released` reads one row of a query whose last row is
  invalid and expects the error. Every late-error case in `_direct_rule_fixtures.py` has one group (`_r1_late_error`,
  `_s3_late_error`, ...), so a rule that checks group by group above a blocking window passes it; S1's fixture has no
  late error at all. **Ruling (owner, 2026-11-11).** Each rule keeps every row checked before the first row leaves; the
  ranked-prefix builder (FRM-18) places the checks for the rules built on it, and this case catches a miss in any
  rule.
- **Change.** Each fixture whose rule takes a `by` or `per` key gains a case whose invalid row sits in the last group.
  S1's case ships with S1-24. `../architecture/rules.md` § The standard tests every rule ships names the grouped case.
- **Needs.** S1-24 (its case is the pattern).
- **Done when.** Each new case fails when its rule's checks sit above the blocking operator (tried on one rule);
  `./test/decide/run_tests.sh -n 2 -k "every_input_row_is_read"`.

## FRM-13 — One copy of the limit-reading code

- **Problem.** Four pieces of shared work are written per rule:
  - N1's `ProveLimitValue` repeats `DirectReadLimit` (`direct_bound_reader.cpp`).
  - The choice between a group bound the rewrite left and the tightest per-row value (`DirectTightestBound`) is
    written in S3 (`limit_bound`), R1 (`limit_bounds`) and N1.
  - The loop that lays out a group bound's inputs (`DirectGroupInputs`) as columns of a `DirectProjectionStage` is
    written five times: S1 twice, S3, R1, N1.
  - S1, S3, R1 and N1 each copy the fields of the `DirectSharedScope` they prove into their `Condition`.
- **Change.** N1 calls `DirectReadLimit`. `DirectDataLimit` gives its group bound itself. One shared function lays out
  a group bound's inputs. Each `Condition` holds its `DirectSharedScope`. Behavior stays as it is (§ Removing bloat).
- **Done when.** `build/release/test/unittest "[decidb]"`;
  `./test/decide/run_tests.sh -n 2 -k "direct_s1 or direct_s3 or direct_r1 or direct_n1 or rule_conformance"`.

## FRM-14 — One overlap test over every pair of rules

- **Problem.** The rule driver raises an internal error when two rules apply to one query.
  `test_decidb_direct_decline.cpp` checks that none do with one hand-written `TEST_CASE` per rule for N1, A2, O1 and
  S3. Six ordered pairs are never tested: S1, A1 and R1 against one another. Each new rule adds its pairs by hand and
  can miss one.
- **Change.** One table-driven `TEST_CASE` runs every rule against every other rule's applying shapes (keyed shapes
  included); the per-rule overlap cases go.
- **Done when.** Every rule in the table is tested against every other rule in both directions;
  `build/release/test/unittest "*never apply to the same query*"`.

## FRM-15 — One interval minimizer for A1 and A2

- **Problem.** A1 and A2 each find the least `a*c^2 + b*c` on an interval `[lower, upper]` (A1's `ComputeAnswers`;
  A2's `EndOfInterval` and the clamp in its P4). The convex case is shared (`DirectTurningPoint`); the concave and
  linear cases are written twice and answer a tie differently. On two equally good ends, A1 takes the lower end, A2
  the end nearest 0. On a flat objective, A1 takes the lower end, or `min(0, upper)` when the box is open below; A2
  takes the point of the interval nearest 0. **Ruling (owner, 2026-11-11).** A tie takes the point of the interval
  nearest 0: one rule for both kinds of tie, with no fallback for an open end.
- **Change.** One shared function, `DirectMinimizeOnInterval`, beside `DirectTurningPoint`, returns the minimizer with
  the ruled tie rule; A1 and A2 call it. A1's tie answers change: read each test and golden diff first.
- **Done when.** A test per rule reaches each tie (two equal ends, a flat objective) and expects the point nearest 0;
  `./test/decide/run_tests.sh -n 2 -k "direct_a1 or direct_a2"`.

## FRM-16 — Remove the solver-order code from S1 and R1

- **Problem.** S1 and R1 order a group's failing checks as the solver does
  (`test_s1_raises_the_checks_of_two_clauses_in_the_solvers_order`,
  `test_r1_raises_the_checks_of_two_clauses_in_the_solvers_order`); S3, A1, A2, N1 and O1 do not. The order picks
  between two true errors, and `../architecture/rules.md` lists error order as something a rule leaves out.
  **Ruling (owner, 2026-11-11).** Error order is not owed: a rule raises when the solver raises, with any of the
  failing checks' messages.
- **Change.** S1 and R1 raise a group's checks in the order the plan meets them, as S3 does. The two tests that reach
  only the ordering go (`.claude/CLAUDE.md` § Testing). `done.md` drops the sentence on the solver's order. A test that
  fails another way: stop and report it.
- **Done when.** `build/release/test/unittest "[decidb]"`;
  `./test/decide/run_tests.sh -n 2 -k "direct_s1 or direct_r1"`.

## FRM-18 — Build the ranked-prefix plan builder; S3 uses it

- **Problem.** S1 and S3 build one plan: rank each group's rows by one column (S1 by score, S3 by weight), keep a
  prefix of the ranking (S1 the best k, S3 the longest run that fits the budget), check every row, answer 1 inside the
  prefix. Each builds it alone, and the copies differ: S1's checks sit above its blocking window and S3's below
  (`../rules/s1_uniform_matroid/todo.md` § S1-24); S1 reads group limits with a second read and a join, S3 in its rank
  window (§ S1-26). S2, S4 and R2 (`../rules/*/definition.md`) take the same plan shape. **Ruling (owner,
  2026-11-11).** One shared ranked-prefix builder (Template Method: the builder fixes the plan, a rule gives the
  ranking column and the prefix test); S3 uses it first, then S1 (S1-26).
- **Change.** A ranked-prefix builder in the shared plan fragments: rank within the scope by a given column, a prefix
  by count or by running sum, the group limits, the checks below the blocking window and the whole-table verdict
  (the FRM-12 ruling). S3 builds its plan with it. S3's answers, errors and goldens stay; its plan cost is measured
  against Gurobi on S3's benchmark cases and stays within the noise (§ Small gains).
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct_s3 or rule_conformance"`;
  `build/release/test/unittest "[decidb]"`.

## FRM-20 — The DECIDE canonicalizer reads a sum of constant coefficients as a count

- **Problem.** `SUM(2*x) <= 7 MAXIMIZE SUM(p*x)` over `BOOL` x picks the best three rows, a count bound, and no
  direct-solve rule takes it: S1 reads a count only when the coefficients add up to exactly 1 or -1, and S3 takes only
  a count objective. Each rule adds up the coefficients itself and keeps its own list of the totals it reads: S1's
  `ReadCountBody` (1 or -1, the comparison flipped for -1), S3's `ProveWeights` (declines 1 and -1 for S1), and
  `DirectConstantSlope`. **Ruling (owner, 2026-11-12).** The shared DECIDE canonicalizer
  (`src/planner/decide/decide_canonicalizer.cpp`) rewrites a constraint `SUM(c*x) op B`, whose coefficients add up to
  one nonzero constant c, into `SUM(x) op B/c`, the comparison flipped when c < 0, for the solver path and direct
  solve alike.
- **Change.** The canonicalizer divides the limit by c, a constant or a value read from the data, and flips the
  comparison when c < 0. S1 takes the shapes with no code of its own; S3's check that leaves a count to S1 reads a
  coefficient of 1. The rules' own coefficient sums that the rewrite makes unreachable go. The solver path reads the
  rewritten row: its answers at the edge (B an exact multiple of c, one just off, c = 0.1 with B = 0.3), its error
  texts and its diagnostics are compared with the code before the change on Gurobi and HiGHS. An error text that
  differs, a diagnostic that quotes the rewritten row or a golden diff: stop and report it. A `decidb-architect`
  review of the rewrite comes first (§ How to work any task).
- **Done when.** Oracle tests for `<=`, `>=` and `=` with c > 0 and c < 0 at the edge values, on both paths;
  `build/release/test/unittest "[decidb]"`;
  `./test/decide/run_tests.sh -n 2 -k "canonicalize or like_term or direct_s1 or direct_s3"`;
  `DECIDB_FORCE_SOLVER=highs ./test/decide/run_tests.sh -n 2 -k "canonicalize or like_term"`.
