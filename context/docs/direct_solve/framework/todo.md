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
- **Decision needed.** The `decidb-architect` review (2026-11-12) finds the ruled placement meets every stop condition
  above. The canonicalizer only respells a clause, and three things rest on that: a rebuilt node takes the location of
  the node it equals (`decide_source_provenance.hpp`), the repair offset relaxes the canonical and the written row by
  the same amount (`decide_source_info.hpp`), and the solver's rows stay as they are. Dividing B breaks all three:
  DIAGNOSE quotes `sum(x) <= 3.5` and measures its slack in counts, a NaN or too-large bound message names `cap / 2`,
  `BETWEEN` halves with c < 0 stop merging, golden `corpus.sql:84` (`2 * SUM(x) <= 10`) changes its row, and the
  solver's 1e-6 row tolerance scales by |c| (`SUM(2000000*x) <= 5999999` becomes `S <= 2.9999995`, which accepts 3).
  Undoing that takes a scale channel in `ConstraintProvenance` read by six renderers.
  The review recommends the same rewrite in direct solve's front end (`DirectIR::Read`, `direct_ir.cpp`): the
  constraint keeps B as written and carries `limit_factor` = 1/c, the comparison flipped when c < 0; every rule sees
  `SUM(x)`; S1's `ReadCountBody`, `SourceBound::negated` and S3's coefficient sum in `ProveWeights` go;
  `DirectConstantSlope` stays (per-row clauses read it). Degree 1 and SUM only, outer factors (`2*SUM(x)`) included;
  c = 0, two decisions and a data coefficient leave the constraint alone. The solver path, diagnostics and goldens do
  not change. Recommendation: that placement, which reaches the ruling's aim (no rule keeps its own coefficient sums).
  A second call either way: `SUM(0.1*x) <= 0.3` divides to 2.9999999999999996, which S1 reads as 2 while the solver
  accepts 3 within its tolerance. Recommendation: decline a constant limit whose quotient lies within
  `DECIDE_INTEGRALITY_TOLERANCE` of an integer and is not one (one check, no case list); the alternative keeps it as
  S1's known difference (`test_s1_direct_reads_bounds_exactly_near_an_integer`).
- **Done when.** Oracle tests for `<=`, `>=` and `=` with c > 0 and c < 0 at the edge values, on both paths;
  `build/release/test/unittest "[decidb]"`;
  `./test/decide/run_tests.sh -n 2 -k "canonicalize or like_term or direct_s1 or direct_s3"`;
  `DECIDB_FORCE_SOLVER=highs ./test/decide/run_tests.sh -n 2 -k "canonicalize or like_term"`.

## FRM-21 — One forwarding of a group bound's input columns

- **Problem.** N1 (`ComputeCoefficients`) and S3 (its P2) each forward the columns `DirectLayoutGroupInputs` laid out
  (the reads and the argument of each input) as new columns of a later stage, in the same loop.
- **Change.** One shared function in the plan fragments forwards them; N1 and S3 call it. Behavior stays as it is
  (§ Removing bloat).
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct_n1 or direct_s3 or rule_conformance"`.
