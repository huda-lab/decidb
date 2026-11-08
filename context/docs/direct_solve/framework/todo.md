# Shared engine — open work

The engine is everything that is not one problem class: IR, rule driver, plan fragments, runtime checks, bound reader,
result adapter, policy.
What exists today is in `done.md`. Direct solve declines each language construct it cannot yet model; the
solver path answers those queries, so none of them is wrong, only slower.

## How to work any task in these todo files

- Each task stands alone. Read its **Problem**, make the change and run the checks. When it ships, its result moves to
  the owning `done.md` and the task is deleted from its `todo.md`.
- The baseline to compare against is the stamp at the top of `done.md`. Any other failure after your change is yours.
- Checks that every code task runs (from the repository root): `make decide-test`,
  `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`, `DECIDB_VERIFY_SERIALIZER=1 make decide-test`,
  `DECIDB_FORCE_SOLVER=highs make decide-test`,
  and `build/release/test/unittest "[decidb]"`. Rebuild first with `make release`. `test/decide/run_direct_matrix.sh` runs all
  five and prints one line each (about 3 minutes). Doc-only tasks need no checks.
- Do not loosen or delete a test to make it pass. If a test fails for a reason the task did not predict, stop and report it.
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

## H-48 — A2's group-then-join plan is measured

- **Problem.** A2 computes each class's answer once and joins it back (`../rules/a2_single_facility_location/done.md`
  § How it works). A2's § Performance still holds the window plan's figures, and only `sf=1` was timed on the new
  plan, under load (median of 5): `a2` 0.032 s on all threads against 0.046 s on one; Gurobi took 0.075 s on the old
  sweep.
- **Change.** Run the sweep on a quiet machine and rewrite A2's § Performance from it, with the plan's explanation.
- **Done when.** `a2` runs faster on all threads than on one and is ahead of Gurobi at every size
  (`benchmark/decide/direct_solve/run.py --sf 0.125 0.25 0.5 1 --queries a2 a2_keyed a2_abs`, and the same with
  `--modes direct highs`).

## H-49 — N1, S1 and R1 take the group-then-join plan where it is faster

- **Problem.** N1's query-wide plan has A2's shape (H-48): `n1` times the same on all threads and on one (0.080 s and
  0.079 s at 800,000 rows). S1's and R1's per-group plans repeat a group's values on every row through a window too,
  and are not measured against the group-then-join shape.
- **Change.** N1 takes H-48's shared fragment. S1's and R1's per-group plans are measured against it on their
  benchmark queries, and move only where it is faster.
- **Per rule (architect review of H-48).** N1: the class plan is an aggregate over the first read's P1 to P4
  (`sum` of the squared length, the validity and the checks on `h`), with the data-limit window under the aggregate as
  A2's median window is; the second read runs scope to P3 for each row's target, then the join, then P5. S1: the
  answer is a rank inside the group, so only the group bounds (`AddGroupBounds`) can move to the aggregate and join;
  measure that. R1: already group-then-join; its `JoinOnKey` becomes `DirectJoinClassAnswers`, and its window plan is
  the one to measure. `ClassAggregate::Add` (A2) and R1's `Aggregate` bind one named aggregate over one argument: one
  shared helper, the hash counterpart of `DirectWindowExtremum`. A2's absolute loss over a query-wide decision keeps a
  window with no `PARTITION BY` for its running sum: measure a query-wide `a2_abs` shape, and move it where a parallel
  plan is faster.
- **Needs.** H-48.
- **Done when.** `n1` runs faster on all threads than on one; each of S1 and R1 either moves with a faster time or keeps
  its plan with the measurement recorded in its `done.md` § Performance; the matrix passes with no answer changed.

## TEST-2 — Direct-solve arms no test reaches

- **Problem.** At `73b52ab1d1`, `make decide-coverage` reaches 2,397 of the 2,450 arms in `direct/*.cpp` and
  `decide_rewrite_rhs_bounds.cpp`; `coverage_report.py report --unreached` lists the other 53. Three are kept on
  purpose (`done.md`, branch coverage). Two groups are tests that miss what they are for:
  - `DirectSourceReader::CopyOfChain` (`direct_source_reader.cpp`, the re-read of a projection or filter chain, five
    arms): `test_r1_sources_over_a_stored_table_agree_with_the_solver` names it as its purpose ("a source is re-read
    when it is a chain of projections and filters over one table scan"), but under the copy rule (§ Row checks) its
    2,000-row sources are copied, so no test takes the re-read.
  - N1 with a limit input that has no argument, `SUM(POWER(x, 2)) <= COUNT(*)` with or without `by` (two arms in
    `n1_euclidean_ball_rule.cpp`): the rule takes it under `require`, and no test runs it.
  - The nested-reducer reading (`DirectPart::inner`, set in `ReadReducer`, `direct_ir.cpp`) and every check of it
    (`DirectPart::IsPlainSum`, A2's `plain`, the IR's own decline): the binder rejects a nested reducer everywhere but
    the two-level objective `outer(per k: inner(e) by (k))`, and `ReadObjective` declines that first
    (`IsTwoLevelObjectiveWrapper`), so no query reaches it. It is deleted with the IR test that only names it; the
    two-level objective stays a decline (`../architecture/ir.md` § Not proved), and the rule that first takes it reads
    it into the IR.
  The others are declines and guards in the IR readers, the bound reader, the plan fragments, late materialization and
  the S1, A1 and A2 rules.
- **Change.** For each arm: add a test that reaches it (oracle-compared where it answers), or delete it as code no
  query reaches (owner ruling 2026-10-21), or keep it as a rule-handoff or typed-read guard and name it in `done.md`.
- **Done when.** `--unreached` lists only the arms `done.md` names, and the matrix passes.
