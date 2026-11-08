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

## H-06 — The two-level objective `outer(per k: inner(e) by (k))`

- **Problem.** The objective is `MINIMIZE sum(per id: max(w*x) by (id))`. `ReadObjective`
  (`direct_ir.cpp`) declines on `IsTwoLevelObjectiveWrapper`. The nested-reducer reading (`DirectPart::inner` in
  `ReadReducer`) exists, but no query reaches it: the binder rejects a nested reducer anywhere else.
- **Change.** Read the two-level objective into the IR through the nested-reducer reading.
- **Done when.** `test_decidb_direct_ir.cpp`, "declines a two-level objective", reads the inner reducer. The nested order-0 norm case returns: an inner not-proved part leaves the outer not proved.
- **Needs (owner, 2026-10-29).** A rule that takes the shape. Read into the IR with no rule taking it, the reading was
  reached only by an IR test, so it was removed; this task waits for a rule that takes the shape (A3 or A4,
  `../todo.md`).

## H-08 — VALUE-shape clauses (helper decisions)

- **Decision needed.** The rule and clause shape this is for. No rule's definition takes a VALUE-shape clause, so
  reading the helper into the IR alone reaches no code.
- **Problem.** `ReadConstraint` (`direct_ir.cpp`) declines a leaf tagged `VALUE_REWRITE_TAG`. The solver builds a
  helper decision from `reducer_value_links`.
- **Options.** 1. Leave it until a rule names the shape it needs; the task then becomes that rule's. 2. The owner names
  a rule and shape now.
- **Recommendation.** 1.
- **Change.** Read the helper decision and its definition into the IR, so a rule that applies can take the clause.
- **Done when.** A differential case with a VALUE-shape clause runs on both paths with the same answer.

## H-48 — A2 computes a class's answer once per row, and a query-wide window runs on one thread

- **Decision needed.** Whether a rule that repeats one answer per class computes it once per class and joins it back,
  in place of a window that repeats the class's sums on every row.
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
- **Recommendation.** 1, as a shared plan fragment beside `DirectProjectScope` and `DirectWindowSink`, taken by A2
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

## BLOAT-1 — Bloat in the direct-solve engine

- **Problem.** The engine (`src/optimizer/decide/direct/` apart from the rule files, its headers, the C++ tests in
  `test/common/`, and the shared tests `test_direct_user_facing.py`, `test_direct_rule_conformance.py`,
  `test_direct_source_values.py`, `test_direct_binding_boundary.py` and `_direct_*.py`) has not been read for bloat
  since five rules grew on it. Known cases:
  - `DirectAbsTerm` and `DirectSquareTerm` hold the same fields (A2-10 owns that one).
  - A rule copies a shared fragment: R1's aggregate stage repeats `DirectAggregateOver`
    (`direct_plan_fragments.cpp`); R1's `JoinOnKey` (a SINGLE join on `IS NOT DISTINCT FROM`, else a cross product)
    repeats the join in `direct_source_reader.cpp`; R1's `SignedValues` has near twins in S1
    (`s1_uniform_matroid_rule.cpp`) and in `direct_plan_fragments.cpp`; A1's `PlanBuilder::Passthrough` and N1's
    `PlanBuilder::Forward` are one stage-forwarding helper, and A2 may hold a third.
  - Missing includes hidden by the unity build: `direct_bound_reader.cpp`, `direct_expression.cpp` and
    `direct_result_adapter.cpp` use `BoundConstantExpression` (and A1's other `bound_*_expression.hpp` types) without
    including them, so A1 keeps five includes it does not use; `n1_euclidean_ball_rule.cpp` does not compile alone.
  - Shared tests that repeat a rule's: `test_a_tiny_square_factor_warns_under_a1_and_n1_alike` (its A1 half) runs the
    SQL of A1's `test_a1_coefficients_that_keep_their_precision_are_silent` and
    `test_a1_warns_when_a_coefficient_beside_a_square_leaves_the_normal_range`; A1's `late_unbounded_row_under_limit`
    runs the late-row error of A1's `late_errors` fixture entry; the no-Gurobi environment (strip
    `GRB_LICENSE_FILE`, `HOME`, `DECIDB_FORCE*`) is built in two N1 tests, `test_quadratic.py` and
    `test_direct_user_facing.py`.
- **Change.** Remove the bloat by § How to work any task (Removing bloat). The rule files are BLOAT-2 to BLOAT-5 and
  A2-10 to A2-12.
- **Done when.** The matrix passes with no answer, error text or golden changed, and the report lists each removal
  with its evidence and the area's lines and `lizard` figures before and after.

## BLOAT-20 — Decline guards that only an edited IR reaches

- **Problem.** Some decline guards of the rules cannot be reached from SQL, only from the C++ decline test,
  which edits the IR on purpose (`test/common/test_decidb_direct_decline.cpp`, header: "the shapes the front end never
  produces from SQL"). In R1: the `output_type != DOUBLE` test in `ProveDecision` (`direct_ir.cpp:216-222` gives the
  REAL domain only to a DOUBLE output), `ir.decisions.empty()` (the grammar requires a decision), the strict-comparison
  guard in `ProveLimit` (the binder rejects `<` and `>` on REAL), and `var_a >= decision_count` and
  `!term.coefficient` in `OnlyLinearTerms`. S1, A1, N1 and A2 likely hold the same kind. Either they are dead code
  (delete each guard and the C++ cases that reach only it), or they are the rule's own contract on its input, kept
  because a front-end change could start producing the shape (keep them, and the edited-IR cases stay).
- **Ruling (owner, 2026-10-21, H-34 batch 2; applied 2026-11-08).** Unreachable defensive branches are dead code:
  delete them with the tests that only reach them; when a later change makes a shape reachable, handle it then. Two
  kinds stay: rule-handoff checks, and type-safety checks that guard a cast or a typed read.
- **Change.** List the guards of every rule (S1, A1, R1, N1, A2) and of the engine that only an edited IR reaches;
  delete each one that is neither kind above, with the C++ cases in `test_decidb_direct_decline.cpp` that reach only
  it. A guard whose shape SQL can still produce (for example a key-only keyed scope from
  `decide_declarations_binder.cpp:308`) stays.
- **Done when.** The matrix passes with no answer, error text or golden changed.
