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

## H-52 — Statistics prove constants that nothing reads as constants

- **Problem.** At `sf=1` (`benchmark/decide/direct_solve/run.py --compare`, summed operator CPU, direct against the fair
  comparator in `hand/`), a1 stands at 1.86×, a2_abs_global 1.42× and r1 2.01×; n1 at 1.20–1.35× across runs and
  n1_groups 1.29× sit near the line; the rest are at or under 1.19×. The causes, from EXPLAIN and the profiles:
  - **A constant the statistics prove stops where it is proved.** Statistics propagation folds a `CASE` to a constant
    inside a projection (`propagate_case.cpp`), but nothing folds what reads it, because DuckDB's expression rewriter
    and `DirectCanonicalize` both run before statistics propagation. A2's weight
    `CAST(CASE WHEN v IS NOT NULL THEN 1 ELSE 0 END AS DOUBLE)` becomes `CAST(1 AS DOUBLE)`, evaluated per row, and the
    projection above computes `#0 != 0.0`, `abs(#0) < DBL_MIN` and `/ #0` on it per row (9 ms of a2_abs_global's 30).
    A1's clause end folds to `-inf`, and the projection above computes `greatest(0.0, #0)` and `#0 = inf` per row
    (4.5 ms per read).
  - **Statistics stop at a CTE reference.** R1's per-row stage is a `LogicalMaterializedCTE`; its readers are
    `LogicalCTERef`s, which carry no statistics, so their checks over NOT NULL columns do not fold.
  - **A double negation.** `DirectScaledTerm` (`direct_plan_fragments.cpp`, `sign < 0`) negates a coefficient in one
    projection, and N1 (`calc.Neg(b)`) and A2 (`calc.Neg(constant)`) negate it again one projection higher (about 4 ms
    of n1).
  - **R1's rounds of aggregate-then-join.** R1's window is 182 ms against the hand plan's 270 ms for two windows; the
    gap is three hash aggregates over 800K rows (76 + 54 + 28 ms) and their joins back (59 ms): the totals, the price
    and the shares. The hand plan's `RANGE` frame gives each row its peer group's sums from the one sort.
- **Change** (commits in this order; re-measure after each):
  1. **Constant folding inside statistics propagation** (core). After an expression's children are propagated, an
     expression whose children are all constants and that is foldable is evaluated
     (`ExpressionExecutor::TryEvaluateScalar`; a failure leaves it, to raise at run time). Tested on the children, not
     with a recursive `IsFoldable` per node. `error` and the warning function are volatile, so they never fold.
  2. **Constant-column propagation inside statistics propagation** (core). A projection column whose expression is a
     constant after propagation, or a reference to a column that is, records its binding and value. A column reference
     to a recorded binding becomes the constant when it sits inside a computation, never when it is the whole
     expression (a key, a sort column, a pass-through stay columns), and only while the binding's statistics exist and
     agree on NULL-ness with the value (a NULL-padded side is marked nullable, so a non-NULL constant stops there).
  3. **Statistics across a materialized CTE** (core). Propagating a `LogicalMaterializedCTE` records its definition's
     column statistics and constants by CTE index; a `LogicalCTERef` to it takes them by position. A reference to a
     `LogicalRecursiveCTE` takes nothing.
  4. **The canonicalizer's constant propagation measured against commit 2.** Delete `DirectCanonicalize`'s
     `ConstantPropagator` and its join-type list if the 11 benchmark plans and their CPU are unchanged; otherwise keep
     it and record in its header which fold needs the constant before the rewriter runs.
  5. **One negation.** A consumer that needs the opposite sign asks `DirectScaledTerm` for it when the term is built
     (algebraic simplification before lowering), where the un-negated term has no other reader.
  6. **R1's peer-group sums from its one window.** In the existing window (same partition and order),
     `RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW` gives a row the sum through its peer group and
     `RANGE BETWEEN CURRENT ROW AND CURRENT ROW` the peer group's own sum, and the totals are full-partition aggregates
     of the same window. Measure the shape that reads them in place of the price and shares aggregate-then-join rounds;
     take it if it wins and every obligation (proportional ties, the anchor, the open-up and open-down checks) holds.
- **Ruling (owner, 2026-11-09, delegated).** (1) The fix is in the statistics propagator (sparse conditional constant
  propagation: constants are the narrowest statistics, and the pass that proves them folds them, as its comparison and
  conjunction handlers do), not a second `DirectCanonicalize` after statistics. (2) A constant replaces a column
  reference only inside a computation: no list of key positions. (3) Commit 4 deletes the duplicate only on a measured
  tie. (4) R1 keeps proportional tie sharing; the `RANGE`-frame shape is measured before full-partition totals alone.
  (5) An arithmetic identity left per row after commit 2 (`x / 1.0`) gets a rule only if its cost is measured.
- **Done when.** Each `run.py` query at `sf=1`, one process, best of 5, within 1.3× of its fair
  comparator in summed operator CPU; on all threads no slower than on one at every size, or no further from that than
  its comparator is; answers, error texts, warnings and the first error to raise unchanged; goldens unchanged; EXPLAIN
  changes in core sqllogictests read and accepted one by one; the matrix passes. A query that misses is reported with
  its profile and the cause.
