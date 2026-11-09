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
  `DECIDB_FORCE_SOLVER=highs make decide-test`, and `build/release/test/unittest "[decidb]"`. Rebuild first with
  `make release`. `test/decide/run_direct_matrix.sh` runs all five and prints one line each (about 3 minutes). Doc-only
  tasks need no checks.
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

## H-48 — A2's group-then-join plan is measured

- **Problem.** A2 computes each class's answer once and joins it back (`../rules/a2_single_facility_location/done.md`
  § How it works). A2's § Performance still holds the window plan's figures, and only `sf=1` was timed on the new
  plan, under load (median of 5): `a2` 0.032 s on all threads against 0.046 s on one; Gurobi took 0.075 s on the old
  sweep.
- **Change.** Run the sweep on a quiet machine and rewrite the § Performance of A2, N1 and S1 from it, with the plan's
  explanation: A2's absolute loss, N1 and S1's group bounds moved to group-then-join after the old figures were taken.
- **Done when.** `a2` runs faster on all threads than on one and is ahead of Gurobi at every size
  (`benchmark/decide/direct_solve/run.py --sf 0.125 0.25 0.5 1 --queries a2 a2_keyed a2_abs a2_abs_global n1 n1_groups
  s1 s1_global r1 r1_global`, and the same with
  `--modes direct highs`).

## H-53 — Two mechanisms that buy nothing measurable

- **Problem.** Statistics propagation folds what it proves and reads a constant column of a projection as its value
  inside a computation (`statistics_propagator.cpp`, `propagate_columnref.cpp`). Two mechanisms beside it measure as
  noise at `sf=1` (`benchmark/decide/direct_solve/run.py --compare`):
  - **`DirectCanonicalize`'s constant-column propagation** (`ConstantPropagator`, the join types it passes constants
    across, `direct_canonicalizer.cpp`) does the same job as the statistics propagator, earlier. Deleting it leaves a
    few `x * 1.0` per row in 8 of the 11 benchmark plans and changes no query's CPU beyond noise; an `x / 1.0` measures
    0.1 ms per 800K rows.
  - **Statistics across a materialized CTE** (`propagate_cte.cpp`, `cte_columns`) moved r1 from 1.80× to 1.75× of its
    comparator, inside the ±15% noise of a loaded machine, and carries an exclusion of its own (a recursive CTE).
- **Change.**
  1. Delete `ConstantPropagator` and its join-type list from `direct_canonicalizer.cpp`, the C++ tests that only reach
     them (`test_decidb_direct_canonicalizer.cpp`), and the header's account of them. If the canonicalizer is then
     DuckDB's rewriter over the operators a rule built, run `Optimizer::rewriter` over the adapter's child as DuckDB
     runs it (`VisitOperator`), and delete `ExpressionRewriter::RewriteOperatorExpressions` (core) and the walk that
     stops at the source, unless a plan of the source then changes; say which in the report.
  2. Delete `propagate_cte.cpp`, its dispatch, `cte_columns` and `statistics_cte.test`.
- **Ruling (owner, 2026-11-09).** Work that adds complexity for a gain inside noise or a few milliseconds of latency is
  not done, and landed work of that kind is removed; a change that simplifies is done. Not done, on that rule: a
  smallest-nonzero-magnitude analysis to drop the below-normal check of a value of an exact type (a2_abs_global, 0.6 ms
  of latency); R1's per-row answer from window frames (a second plan shape for several decisions, and prefix sums the
  segment tree does not keep monotone); proving R1's group checks at plan time.
- **Done when.** The two mechanisms and their tests are gone; answers, error texts, warnings, the first error to raise
  and goldens are unchanged; the benchmark's CPU is unchanged within noise; the matrix passes.
