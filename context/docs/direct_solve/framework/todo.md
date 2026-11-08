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

## H-48 — A2 computes a class's answer once per row, and a query-wide window runs on one thread

- **Ruling (owner, 2026-11-08).** A rule that repeats one answer per class computes it once per class and joins it
  back (option 1), in place of a window that repeats the class's sums on every row.
- **Problem.** A2's plan (and N1's, the same aggregate-then-repeat shape) puts the class's sums in a window, then
  computes the answer, its clamps and its checks in a projection over every row: one long `CASE` chain evaluated
  800,000 times at `sf=1` for one query-wide answer. A window with no `PARTITION BY` also hands its rows to a single
  thread, so everything above it runs on one core: `direct` and `direct, 1 thread` time the same for `a2` (0.094 s)
  and for `n1` (0.080 s and 0.079 s at 800,000 rows). Measured with the profiler on `a2` at `sf=1`: the window takes
  15 ms and the answer's projection 35 ms of 71 ms. The solver path takes 50 ms, as it folds the shared decision into
  one variable whose coefficients are the same sums; A2's `a2` query is 0.8 to 0.9 times Gurobi's speed
  (A2 `done.md` § Performance). The keyed plan does the same per-row work in parallel and stays ahead. The same
  projection carries constant branches DuckDB leaves unfolded (`CASE WHEN false THEN true ...`, read in the
  profiler's plan), which computing the answer once per class would also take off the per-row path.
- **Options.**
  1. Group-then-join: aggregate per class (a hash aggregate on the key, or an ungrouped one for a query-wide
     decision), compute the answer and its checks on the aggregate's rows, and join the answer back to the rows on the
     key (a cross join of one row for a query-wide decision). The source is read once into a materialized CTE and
     scanned twice, as DuckDB does for a CTE referenced twice, so a source with `random()` or a costly join is not
     computed twice. The join's build side consumes every row before the probe releases one, so each row's value check
     still runs on every row first. Plain-SQL sizing at `sf=1` with a light answer: 26 ms against 3 to 4 ms
     query-wide, 15 ms against 7 ms keyed.
  2. Keep the window and leave it. Cost: A2's query-wide squared loss stays slower than the solver.
- **Change.** Option 1, as a shared plan fragment beside `DirectProjectScope` and `DirectWindowSink`, taken by A2
  here; N1, S1 and R1 follow in H-49. It changes A2's plan skeleton's W and P4 stages, not any loss.
- **Done when.** `a2` runs faster on all threads than on one and is ahead of Gurobi at every size
  (`benchmark/decide/direct_solve/run.py --sf 0.125 0.25 0.5 1 --queries a2 a2_keyed a2_abs`), and the matrix passes
  with no answer changed.

## H-49 — N1, S1 and R1 take the group-then-join plan where it is faster

- **Problem.** N1's query-wide plan has A2's shape (H-48): `n1` times the same on all threads and on one (0.080 s and
  0.079 s at 800,000 rows). S1's and R1's per-group plans repeat a group's values on every row through a window too,
  and are not measured against the group-then-join shape.
- **Change.** N1 takes H-48's shared fragment. S1's and R1's per-group plans are measured against it on their
  benchmark queries, and move only where it is faster.
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
