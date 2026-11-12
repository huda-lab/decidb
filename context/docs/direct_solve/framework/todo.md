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
  Not done either: reading `SUM(c*x) op B`, the coefficients adding up to a constant c other than 1 or -1, as the
  count `SUM(x) op B/c` (FRM-20). The solver path answers it correctly, a user who means a count writes `SUM(x)`, and
  the rules' checks for 1 or -1 are a fixed list (the sign). Dividing by c brings a rounding edge (`SUM(0.1*x) <= 0.3`
  gives 2.9999999999999996) that needs a decline of its own; in the shared canonicalizer the division also moves the
  solver's rows, tolerance and diagnostics. **Ruling (owner, 2026-11-12).**

## FRM-22 — Group checks hold every row in a materialized stage, not a one-thread window

- **Problem.** A ranked-prefix plan whose group can fail closes with W3, `bool_and(verdict) OVER ()`
  (`direct_ranked_prefix.cpp` CloseGroups). DuckDB runs a window with no partition and no order on one thread, so the
  rows above it leave on one thread too. W3 is there only to hold every row before the first leaves (§ the all-rows
  rule in `../architecture/rules.md`); the verdict it computes is the AND of a check every row already carries. SQL
  mimics of the plan at TPC-H sf 1 (800,000 rows): by key 52 ms with W3, 19 ms with the check raised per row under a
  `MATERIALIZED` CTE, 18 ms with no check; `s1_global` 75 ms with W3 and 33 ms with the CTE. A late group's error
  still raises under `LIMIT 1` with the CTE.
- **Change.** The group verdict becomes a per-row check (`DirectRaiseUnless` of the group's verdict) in the stage the
  verdict reads, and the window over all the rows becomes a materialized row stage above the last stage the checks
  read: a `LogicalMaterializedCTE` with `CTE_MATERIALIZE_ALWAYS` read by one `LogicalCTERef`, as `DirectSourceReader`
  builds its copy. The stage stands wherever a check above the rank window needs one (the D_ASSERT in CloseGroups).
  S3's W2 (`PrefixSize`) computes a value and stays a window; S3's group checks go under the stage above it. The
  rows leave in the order the stage gives them; a test that reads the order of a direct-solve result is a stop and
  report. `GroupVerdict` keeps its meaning (whether the row's group can be met); only where the builder raises it
  moves. A `decidb-architect` review of the stage comes first (§ How to work any task).
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct_s1 or direct_s3 or every_input_row_is_read or
  direct_user_facing or direct_class_key"`; `build/release/test/unittest "[decidb]"`; `python3
  benchmark/decide/direct_solve/run.py --compare --sf 1 --queries s1 s1_global s3 s3_global` against the S1 and S3
  `done.md` Performance tables.

## FRM-23 — One shared linear score for the ranked rules

- **Problem.** S1 reads its objective `SUM(score*x)` and lays out the score with code only S1 has
  (`s1_uniform_matroid_rule.cpp`: `ProveObjective`, `ScoreCanFail`, `PartIsFiniteConstant`, `ScoreMessage`,
  `CombinedScore`, about 180 lines). S2 reads the same objective and would copy it.
- **Change.** Move the proof (a condition part) and the P1 layout (the score column and its messages) into shared code
  (`DirectLinearScore` in `direct_plan_fragments`), with S1 its first caller. Behavior, error texts and plans stay as
  they are.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct_s1"`; `build/release/test/unittest "S1*"`.

## FRM-24 — One reader for count limits

- **Problem.** Limits are read two ways. S3, R1 and N1 read theirs with `DirectReadLimit` (`direct_bound_reader`); S1
  has its own (`SourceBound`, `NormalizeCapacity`, `ProveBounds`, and the rounding to whole rows in `SourceLimit`
  and `UpperLimit`). S2 reads one count limit per level and would need S1's copy.
- **Change.** One count-limit reader built on `DirectReadLimit`, and one shared fragment that turns a limit into an
  inclusive whole number of rows (ceil for a lower side, floor for an upper one, the 2^53 bound). S1 reads its limits
  with them, and its own reader goes. Answers, error texts and the near-integer behavior
  (`test_s1_direct_reads_bounds_exactly_near_an_integer`) stay as they are.
- **Needs.** FRM-23 (both edit S1's condition).
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct_s1 or direct_s3 or direct_r1 or direct_n1"`;
  `build/release/test/unittest "[decidb]"`.

## FRM-25 — S1 and S3 share the count body, the decision shape and the split of fixed rows

- **Problem.** The line between S1 and S3 (constant coefficients adding up to 1 or -1) is written twice: S1's
  `ReadCountBody` and S3's `ProveWeights`, which sums the coefficients itself beside `DirectConstantSlope`. The
  decision-shape test and the split of the clauses into fixed rows and coupling constraints are copies in the two
  rules. S1 counts a group's rows with a COUNT(*) of its own beside `DirectWindowMatchingCount`.
- **Change.** One `DirectReadCountBody` both rules read, so S1 and S3 are disjoint by one test; one shared decision
  shape test and one `DirectSplitFixes`; S1 counts with `DirectWindowMatchingCount`. This is a BLOAT task
  (§ Removing bloat): record the lines and `lizard` figures of S1 and S3 before and after.
- **Needs.** FRM-24 (both edit S1's condition).
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct_s1 or direct_s3"`; `build/release/test/unittest
  "[decidb]"`.

## FRM-26 — The ranked-prefix builder ranks over several nested levels

- **Decision needed.**
- **Problem.** The builder (`direct_ranked_prefix`) holds one group key throughout: one `DirectSharedScope` and one
  `scoped` flag, one scope projection, one key in `DirectCarried`, one rank in W1, `PartitionByGroup` and `OverRank`
  over that key, and one eligible flag in the group checks. S2 ranks a chain of nested levels: each level ranks the
  rows the finer level kept (`../rules/s2_laminar_matroid/todo.md` § S2-2), so it needs one rank per level. The hooks
  also hand their checks through the base class's `checks_below` and `checks_above`, which work only in a fixed call
  order and become a per-level protocol with several levels.
- **Options.**
  1. Levels inside the builder: a `vector<Level>` (scope, carried key, rank, group bounds, the flag of a row still
     kept); S1 and S3 are one level and S2 is several. The rank of level k is a running count of the rows level k-1
     keeps, over level k's partition. No case list; S1's rank is the one-level chain. S3 holds a level count it never
     uses above one.
  2. S2 builds its own chain, and the builder's fixed rows, group checks and answer move out as shared fragments.
     The builder stays one level; S2 repeats its skeleton.
- **Recommendation.** Option 1.
- **Change.** The option the owner rules. The hooks return their checks instead of pushing to the base class's
  vectors. S1 and S3 keep their plans: one level gives the plan it gives today.
- **Needs.** FRM-22 (both rewrite CloseGroups), FRM-25.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct_s1 or direct_s3 or every_input_row_is_read or
  direct_user_facing or direct_class_key"`; `build/release/test/unittest "[decidb]"`.

## FRM-27 — The scope proof reads one clause, compares two, and compares keys without order

- **Problem.** `DirectProveSharedScope` (`direct_plan_fragments.cpp`) proves that every clause of a list has one
  scope, all or nothing. S2 reads one scope per level and compares them, and `DirectSameKey` (`direct_rule.hpp`)
  compares keys in order, while nesting reads one key as a subset of another.
- **Change.** Split it into reading one clause's scope (`DirectReadScope`) and comparing two (`DirectSameScope`), with
  `DirectProveSharedScope` their composition, and a key comparison that ignores order, which `DirectSameKey` and S2's
  nesting proof read. S1 and S3 keep their answers and declines.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct_s1 or direct_s3 or direct_class_key"`;
  `build/release/test/unittest "[decidb]"`.
