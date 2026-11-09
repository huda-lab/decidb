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
- **Change.** Run the sweep on a quiet machine and rewrite the § Performance of A2, N1 and S1 from it, with the plan's
  explanation: A2's absolute loss, N1 and S1's group bounds moved to group-then-join after the old figures were taken.
- **Done when.** `a2` runs faster on all threads than on one and is ahead of Gurobi at every size
  (`benchmark/decide/direct_solve/run.py --sf 0.125 0.25 0.5 1 --queries a2 a2_keyed a2_abs a2_abs_global n1 n1_groups
  s1 s1_global r1 r1_global`, and the same with
  `--modes direct highs`).

## H-50 — A direct plan costs two to three times DuckDB's own plan for the same answer

- **Problem.** At `sf=1` (800,000 rows), a direct plan takes two to three times what DuckDB takes for a hand-written
  query that computes the same answer, and the gap is the plan's shape, not the engine: the same relational plan
  written by hand costs the same. Measured with `PRAGMA enable_profiling='json'` (latency / summed operator CPU):
  - `a2_keyed` 25 ms / 73 ms against 16 ms / 38 ms for `WITH g AS (SELECT ps_suppkey, avg(...) FROM partsupp GROUP
    BY ps_suppkey) ... JOIN g USING (ps_suppkey)`. The class join (`DirectJoinGroupRow`, `direct_plan_fragments.cpp`)
    is a SINGLE join on `IS NOT DISTINCT FROM` over two keys, the row's eligibility flag and the key, and takes 20 ms
    of CPU where DuckDB's inner join takes 1.6 ms: DuckDB's perfect hash join takes only an inner join on one integer
    key compared with `=` (`PerfectHashJoinExecutor::CanDoPerfectHashJoin`), and its perfect hash aggregate takes
    only integer group keys (`CanUsePerfectHashAggregate`), so the BOOLEAN flag keeps the class aggregate a radix hash
    aggregate too. The flag is the constant `CASE WHEN false THEN false ELSE true END` when no key element can put a
    row in no class. At 100,000 rows the plan runs slower on all threads than on one (34 ms against 30 ms, peak 83
    MiB against 38 MiB), where DuckDB's plan does not.
  - `a2` 26 ms / 83 ms against 8.5 ms / 22 ms for a cross product with `(SELECT sum(c) / count(*) FROM partsupp)`.
    About 70 ms of the CPU is the projections over the 800,000 rows.
  - No expression the rules build is simplified: the DECIDE optimizer runs after DuckDB's `EXPRESSION_REWRITER`
    (`optimizer.cpp`), so constant conditions (`CASE WHEN (true) THEN ... ELSE 0.0 END`), constant terms
    (`"*"(1.0, "+"(0.0, ...))`) and the checks of values a type proves finite (`isfinite(CAST(<DECIMAL> AS
    DOUBLE))`) are evaluated on every row.
  The SINGLE join is deliberate (the comment in `DirectJoinGroupRow`): an inner join is estimated from the
  aggregate's child cardinality, and DuckDB then builds on the rows.
- **Change.** Make each direct plan the plan DuckDB would choose for the same answer written as SQL: simplify the
  replacement the way DuckDB simplifies a query (the expression rewriter, one pass, no case list), put a flag in the
  key only where it can be false, and join the class answers in a form DuckDB runs as a perfect hash join where the key
  allows one, keeping the class plan the build side. Each piece names the standard technique it follows (constant
  folding after lowering, as a compiler's back end does).
- **Design (architect review, 2026-11-09).** Measured at `sf=1` in one process: the class join takes 20.8 ms of CPU as
  built, 15.2 ms as a LEFT join on `=` and 2.1 ms as DuckDB's perfect hash join; the rewriter alone saves about 10% of
  the row-side projections, since DuckDB's arithmetic rules match integer types only. Commits, in order:
  1. **Cheap checks.** `DirectRaiseMessage` gates on the BOOLEAN `number IS NULL OR isfinite(number)` and builds the
     message only in its error arm (today it builds the VARCHAR on every row and tests it for NULL).
  2. **Type-derived finiteness** (abstract interpretation over magnitudes, seeded by types; `overview.md`'s "Prove,
     don't guess"): one `DirectMagnitudeBound(expr)` (a constant's |c|; an exact numeric type's largest magnitude; FLOAT
     and DOUBLE infinite). `DirectIsFiniteWhereKept` asks "Σ bound × scale < DBL_MAX", so `DirectCheckNumber`, S1 and
     R1 drop checks no value can fail; `DirectRowBoundMessage` drops `isfinite`/`isnan` on exact types. FLOAT/DOUBLE
     inputs and every too-large check of a product a rule computes stay.
  3. **Canonicalization after lowering** (MLIR's canonicalize, LLVM's InstCombine), one post-pass in the driver over the
     rule-built operators, stopping at the source as `StripTagsAbove` does: constant-column propagation (a reference to
     a projection column holding a constant becomes the constant, except across the NULL-padded side of an outer
     join), then `optimizer.rewriter` with the rule set `Optimizer` builds, and sums built without a `0.0` seed
     (`DirectFoldQuadratic`). The IEEE-exact identities `x * 1.0`, `x / 1.0`, `x - 0.0` (not `x + 0.0`, inexact at
     -0.0; A2's final `+ 0.0` turns -0.0 into +0.0 and stays) go into DuckDB's `ArithmeticSimplificationRule` for
     floating types.
  4. **`CaseSimplificationRule` evaluates a constant WHEN with `TryEvaluateScalar`** and leaves the CASE alone when
     evaluation fails, as `ConstantFoldingRule` does: today it calls the throwing `EvaluateScalar`, so a WHEN whose
     evaluation throws raises at plan time, even over no rows.
  5. **Perfect hash join for the probe-outer joins** (SINGLE, LEFT) in DuckDB's `PerfectHashJoinExecutor`, with one
     all-NULL sentinel slot past the build range that an unmatched or NULL probe key points at. The SINGLE join stays
     (its build side is structural); duplicate build keys keep falling back, so its more-than-one-row error stays.
     sqllogictests: gaps, a NULL probe key, a key out of range, duplicate build keys, an empty build.
  6. **`IS NOT DISTINCT FROM` → `=` in statistics propagation** (`propagate_join.cpp`) when neither side's statistics
     can hold a NULL.
  7. **The class key without the flag** (NULL as the absent-group marker, SQL's outer-join semantics): an element whose
     NULL puts a row in no class is compared with `=`, and is `CASE WHEN eligible THEN key END` where a `when` or an
     instance condition can drop the row; an element where NULL is a value is compared with `IS NOT DISTINCT FROM`;
     only a key with no `no_group` element whose eligibility can be false gets one integer marker,
     `CASE WHEN eligible THEN 0::UTINYINT END`. `DirectProjectScope`/`DirectCarried`, `DirectJoinGroupRow`,
     `DirectJoinClassAnswers`, A2's `ClassKey`, N1, S1 (class key and window partition), R1, and the bound side
     tables, which the solver path shares.
  8. S1's and R1's remaining gap, profiled after 1–7 and closed the same way, then the H-48 sweep.
- **Ruling (owner, 2026-11-09, delegated).** (1) No exact aggregation of exact-typed values (`sum(CAST(x AS DOUBLE))` →
  `CAST(sum(x) AS DOUBLE)`): a DECIMAL sum can overflow where the DOUBLE sum does not, a new error the solver path
  never raises; the bar is DuckDB's hand-written plan doing the same DOUBLE arithmetic. (2) The core edits are taken
  (perfect hash join for SINGLE/LEFT, NOT DISTINCT FROM → `=`, the float identities in DuckDB's rule, the CASE
  rule's fix): each is generic and helps every query. (3) The solver path's side tables take the per-element
  comparison, so one key form exists. (4) The `0.0` seeds go; a message that would print `-0` is a stop. (5) No
  interval propagation through products: no measured payoff. (6) The CASE rule is fixed at its cause (commit 4).
- **Done when.** Each query of `benchmark/decide/direct_solve/run.py` runs within 1.3 times DuckDB's hand-written plan
  that computes the same answer with the same DOUBLE arithmetic, at `sf=1`, measured in one process (`PRAGMA
  enable_profiling`), and on all threads no slower than on one at every size; answers, error texts and the first
  error to raise unchanged, goldens unchanged, and the matrix passes.

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
