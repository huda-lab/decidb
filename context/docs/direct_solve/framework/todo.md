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

## FRM-23 — Constants a rule lays out as columns fold through the plan

- **Problem.** A rule builds its plan from projection stages, so a constant it lays out as a column (N1's `q` of a
  constant slope, a `true` flag such as `in_ball` with no `when`, A1's floor) reaches every projection above it as a
  column reference. The driver runs DuckDB's `ExpressionRewriter` over the installed plan (`direct_rule_driver.cpp`),
  which folds within one expression only. Statistics propagation substitutes a projection's constant columns later
  (`SubstituteConstantColumns`), but folds a node only when all its children are constants, so `x / 1.0`, `x * 1` and
  `CASE WHEN true` stay, and it runs after unused columns are removed. N1 worked around it on its own: `Factor()` and
  `Condition::constant_factor` in `n1_euclidean_ball_rule.cpp` build a literal `q` and four `constant_slope` guards skip
  the columns (`n1` at SF 1: 44.7 ms CPU with `q` a column, 35.3 ms with the literal).
- **Ruling (owner, 2026-11-13).** Design (A), reviewed by `decidb-architect`: one bottom-up pass over the rule's
  installed subtree, `DirectConstantPropagation` (a `LogicalOperatorVisitor` in `src/optimizer/decide/direct/`), in
  place of the driver's rewriter call. At each operator, children first: substitute every column reference that reads
  a known constant column and is not the whole expression; apply the optimizer's own rewriter to the operator; then a
  projection records its constant columns (a constant, or a whole-expression reference to a known one), and a join that
  pads a side with NULL (SINGLE, LEFT, RIGHT, FULL; DuckDB's precedent in `propagate_join.cpp`) forgets that side's.
  Every other operator passes bindings through. This is constant propagation with peephole simplification in one pass
  (SCCP then InstSimplify); a tree's bottom-up order reaches the fixpoint. Calls: `ExpressionRewriter::VisitOperator`
  splits into `VisitOperatorChildren` and a public `RewriteOperator` (no behavior change); the statistics propagator's
  constant-column record and the new pass share one helper; statistics propagation keeps its constant columns (they
  fold constants only statistics find). A constant slope under a `when` is masked to NULL on dropped rows and does not
  fold: accepted, and measured.
- **Change.** Add the pass and its unit tests; delete N1's `Factor()`, `constant_factor`, the `constant_slope` guards in
  the plan and the `in_sum` ternary (N1 lays out `q` the same way in every case; `constant_slope` stays where
  `ProveLimitClause` declines a non-positive or non-finite factor); delete A1's
  `lowering_whens.empty() ? floor : Col(p2.Add(floor))` special case (`a1_separable_box_rule.cpp`).
- **Done when.** A `[decidb]` C++ test on a hand-built plan with a case for each decision point of the pass (a
  projection records; a constant passes a filter and a window; the padded side of a SINGLE and a LEFT join is
  forgotten; a whole-expression reference stays a column), each failing if its branch is removed; a raising check that
  folds to `error()` raises nothing over an empty source and the same message over one row;
  `build/release/test/unittest "[decidb]"`; `./test/decide/run_tests.sh -n 2 -k "direct"`;
  `python3 benchmark/decide/direct_solve/run.py --compare --sf 1 --queries n1 n1_groups n1_weighted
  n1_weighted_groups a1 a2` within noise of the figures in each class's `done.md`, and one `n1` variant with a `when`
  measured and reported.

## FRM-24 — Every rule carries `eligible`, true when unscoped

- **Problem.** `DirectCarried` carries the eligible flag only for a scoped rule, so N1 (10 places) and R1 (7) write
  `condition.scope.Scoped() ? Boolean(carried.eligible) : calc.Bool(true)`. With FRM-23 a `true` column folds, so the
  flag can always be carried and the ternaries go.
- **Change.** `DirectProjectScope` and `DirectProjectionStage::Forward` always lay out `eligible` (`true` when
  unscoped); delete the ternaries in every rule.
- **Needs.** FRM-23.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct_n1 or direct_r1"`; the `--compare` figures of `n1` and
  `r1` within noise.
