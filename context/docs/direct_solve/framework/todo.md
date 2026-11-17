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
  solver's rows, tolerance and diagnostics. **Ruling (owner, 2026-11-12).** Not done either: a pass after the rules that propagates
  the constants a rule lays out as columns and simplifies the expressions over them (FRM-23, built and measured). Every
  figure stayed within noise (`n1` 1.22 against 1.21, `a1` 1.12 against 1.08, `a2` 1.15 against 1.14), and it cannot
  replace a literal the rule builds with: a scoped rule (a `when`, a `by` key or a `per`) masks each raw value to NULL
  on the rows outside a ball (`DirectOnKeptRows`), so a constant becomes a CASE over a column that no pass folds;
  `n1_groups` went from 1.14 to 1.38 with N1's literal `q` deleted. A constant that decides the plan's shape, such as
  whether a value needs a mask or a check (N1's `q`, A1's floor), is a literal the rule builds with. **Ruling (owner,
  2026-11-13).**
