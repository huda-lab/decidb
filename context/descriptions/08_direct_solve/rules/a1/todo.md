# A1 (independent bounded decisions) — open work

What A1 does today is in `done.md`. The class definition is in `definition.md`. Each task is independent unless it says
otherwise. When one ships, its result moves to `done.md` and the task is deleted from here. How to run the checks, and the
batches that group tasks across S1, A1 and the shared engine, are in `../_harness/todo.md`; read its "How to work any
task" section first.

**A1 is built (2026-10-06).** The class definition is met; the tasks below are decisions and follow-ups, not missing shapes.
A review on 2026-10-07 found one wrong answer (an empty source with a bound the solver rejects before reading rows); it
is fixed in the working tree (`ProveBounds` now records the box the solver sees, and the declined shapes are tested in
`DECLINED_CASES` in `test_direct_a1.py`). A1-01 and A1-04 to A1-07 are the follow-ups.

## A1-01 — Two wording tests were loosened to pass and should be restored

- **Problem.** Registering A1 originally made seven existing tests fail under `auto`. Five of them inspect the solver's
  model dump (`test_canonicalize_idempotence.py` three cases, `test_implied_bounds.py` two cases); direct solve builds no
  model, so they are now pinned to `DECIDB_TEST_DIRECT_SOLVE=off` with a comment, which is correct and needs no more work.
  The other two assert the solver's own wording for a bound that cannot hold:
  `test_error_infeasible.py::TestInfeasibleModels::test_real_upper_below_nonnegativity_is_rejected` (`x <= -1 cannot hold ... non-negative`)
  and `::test_boolean_lower_above_domain_is_rejected` (`x >= 2 cannot hold ... BOOLEAN`). Their regex was widened to also
  accept `|DECIDE optimization is infeasible` so that they pass with the generic message.
- **Why it matters.** The widening is no longer needed and now hides a regression. A1 hands exactly these queries to the
  solver (`ProveBounds` declines them), so `auto` prints the solver's own message again. Checked on 2026-10-07:
  `x(REAL) x <= -1` and `x(BOOL) x >= 2` both print `DECIDE optimization is infeasible: ... cannot hold because ...` under
  `auto` and `off`, on a table with rows and on an empty one.
- **Plan.** In `test/decide/tests/test_error_infeasible.py` put the two `match=` patterns back to
  `r"(?i)x <= -1 cannot hold.*non-negative"` and `r"(?i)x >= 2 cannot hold.*BOOLEAN"` (see `git diff` of that file for the
  exact original). Run the two tests under the default mode.
- **Done when.** The two tests pass with their original patterns under `make decide-test` and
  `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`. Then delete this task and record in `done.md` (task A1-05) that five
  tests are pinned to the solver path on purpose. About 5 minutes.

## A1-02 — The solver is off by one on a bare strict bound against a fraction

- **Problem.** `x > 0.5` on an `INT` gives 2 and `x < 2.5` gives 1; direct solve gives 1 and 2. The six cases are
  `SOLVER_OFF_BY_ONE` in `test_direct_a1.py`, and the solver-path half of each is `xfail(strict=True)`. `done.md` has the
  exact shapes. The cause is in `decide_bound_absorption.cpp`, which folds a bare literal bound into the variable's box and
  moves a strict bound by a whole unit before rounding.
- **Plan.** Fix the solver (not a direct-solve change): round a strict bound against a fraction to the right integer. Then
  every `xfail` in that test turns into a failure that says to remove the tag; remove them and the split.
- **Care.** A1's check for bounds the solver rejects before reading a row copies the solver's strict shift
  (`k + 1` / `k - 1`, in `ProveBounds`). When the solver's rounding changes, that copy must change in the same commit.
  `../_harness/todo.md` H-07 removes the copy; do A1-04 and H-07 first or together.
- **Done when.** The solver agrees with the oracle on all six cases and the tags are gone.

## A1-03 — A wide column read back costs memory in proportion to the input

- **Problem.** The barrier holds every row until the check has run, so a parent that reads a wide column makes direct solve
  hold it: with a 512-byte payload at 1,000,000 rows the peak is 1.06 GiB where an unused payload costs 47 MiB.
- **Not known.** Whether a plan that evaluates the check in a first pass and joins the result back to the source (reading
  the source twice) beats holding the rows. `architecture/result_boundary.md` chose holding over a second read for S1; no
  measurement for A1's wide case exists. The constant-problem variant already streams.
- **Done when.** A measurement at 1,000,000 and 5,000,000 wide rows, and one recorded decision: keep the barrier or change
  the plan. Update the weak-spots paragraph in `done.md`. This is the same question as S1-05; run it once for both as
  `../_harness/todo.md` H-13.

## A1-04 — `BOOL x > 0.5` is now declined, and one test still expects it to be answered

- **Problem.** The suite has one failure: `test_direct_a1.py::test_a1_reads_a_fractional_strict_bound_correctly[bool_strict_lower_fraction]`.
  The case is a `BOOL` decision with the bound `x > 0.5` (and a `MINIMIZE SUM(s*x)` objective). The correct answer is
  `x = 1` (the only value above 0.5). Before the 2026-10-07 fix A1 answered it. The solver does not: it moves the strict bound
  by a whole unit (`x > 0.5` becomes `x >= 1.5`), sees that a `BOOL` cannot reach 1.5, and raises
  `x >= 1.5 cannot hold because x is BOOLEAN`. A1's new check copies that test, so A1 now declines the query and the
  solver's error is what the user gets. Checked 2026-10-07 on the query
  `SELECT id,x FROM (VALUES (1, 2.0::DOUBLE)) t(id,s) DECIDE x(BOOL) SUCH THAT x > 0.5 MINIMIZE SUM(s*x)`: `auto` and `off`
  raise the solver's error; `require` raises `A1_INDEPENDENT_BOUNDS: variable_bound: a bound contradicts the decision's
  built-in domain ...`. Its sibling `x < 1.5` still answers (`x = 1`), because the shifted bound is 0.5, inside the domain.
- **Why it matters.** The test says "direct solve answers this correctly, the solver is wrong"; the code says "direct solve
  hands this to the solver". Both cannot be true. `done.md` ("Where it differs", the sentence about `BOOL`) also still says
  A1 returns the right answer.
- **Decision needed.** Two choices:
  (a) *Decline it* (what the code does now). A1 then matches S1, which also declines a strict bound against a fraction on
  a `BOOL` (`ConstantPinDomain` in `s1_rule.cpp`). The user sees the solver's wrong error until A1-02 fixes the solver. The
  test case moves out of the direct-path half of `SOLVER_OFF_BY_ONE`: keep only its solver-path strict `xfail` (it records
  that the solver is wrong), delete its `require` half with a comment pointing at A1-02, and leave the other five cases alone.
  (b) *Answer it* in A1: special-case a strict bound on a `BOOL` so it is not treated as a built-in-domain conflict. Then
  `auto` and `off` differ on this shape (they already do for `INT`), and A1 deviates from the solver's pre-check.
  Recommendation: (a). It keeps one rule, "if the solver reads a bound differently, A1 declines", and the wrong answer
  disappears when the solver is fixed.
- **Done when.** The suite is green (6 expected failures stay, since the solver-path half is still wrong), `done.md` says
  which choice was made, and A1-02's list of cases matches.

## A1-05 — A1's docs still describe the behavior from before the fix

- **Problem.** After the 2026-10-07 fix, A1 declines a bound the solver rejects before reading a row, so the solver's own
  message appears. The docs still say the opposite: `done.md` "Where it differs" > "Error wording" says A1 prints the
  generic infeasible message for `x <= -1` and `x >= 2` and points at A1-01; the verification stamp at the top of `done.md`
  says "plus the 7 existing tests listed in `todo.md`"; `../_harness/done.md` (first paragraph and "What the second rule
  needed") says the same; `definition.md` "As built" does not mention the decline at all.
- **Why it matters.** Someone choosing between answers will read a claim the code no longer makes.
- **Plan (doc only, after A1-01 and A1-04).**
  1. In `done.md` "What it does not admit", add a row: *a bound that contradicts the variable's built-in domain* (`x <= -1`
     on a non-negative decision, `BOOL x >= 2`), reason: the solver rejects it before reading a row, even on an empty
     source, so the solver reports it. Remove the two wording examples from "Error wording".
  2. Say in `done.md` that five existing tests inspect the solver's model dump and are pinned to the solver path (see A1-01).
  3. Update the `BOOL` sentence in "Where it differs" to the choice made in A1-04.
  4. Add one bullet to `definition.md` "As built": a bound the solver rejects before reading rows is declined.
  5. Rerun all five checks (`../_harness/todo.md` lists them; `test/decide/run_direct_matrix.sh` does it if H-12 has landed)
     and rewrite the stamp (date, commit, counts) in `done.md` and `../_harness/done.md`.
- **Done when.** No doc mentions the seven failing tests or A1-01, and the stamp matches a run you made.

## A1-06 — A NaN constraint coefficient must name a constraint

- **Problem.** See `../_harness/todo.md` H-06. Today A1 reports a NaN or infinite coefficient on the left of a bound as an
  *objective* coefficient. No test checks which noun the message uses, which is how it went unnoticed.
- **Plan.** After H-06 lands, add one test in `test_direct_user_facing.py`: a table where one row has `w = 'NaN'::DOUBLE`
  and the query `DECIDE x(REAL) SUCH THAT w*x <= 5 MAXIMIZE SUM(s*x)`; under `require` the error must contain
  `constraint coefficient`, and a second query with a NaN in the objective (`MAXIMIZE SUM(w*x)`) must contain
  `objective coefficient`. Do not compare the whole message with the solver's: `at row N` is left out on purpose.
- **Done when.** Both assertions pass.

## A1-07 — An equality bound is split in two by copying every field by hand

- **Problem.** In `A1BoundedRule::ProveBounds`, an equality (`x = k`, `w*x = k`) becomes two clauses (at least and at
  most). The code builds the second by copying `decision`, `coefficient`, `coefficient_text`, `rhs`, `rhs_column` and
  `when` one by one (the block that starts "An equality is a lower and an upper bound that may not agree").
- **Why it matters.** A field added to `A1Clause` later is silently missing from the upper half and nobody notices until a
  wrong answer.
- **Plan.** Add `A1Clause Copy() const` to the struct, build the upper clause with it, then change the comparison of each
  half. No behavior change.
- **Done when.** The block is a few lines, and `test_direct_a1.py` and `test_direct_fuzz.py` pass unchanged.
  About 15 minutes.

## Out of the class, no task

`x <> k`, `x IN (...)`, objective `PER`, `AVG`/`MIN`/`MAX` objectives, entity or scalar decisions and coupled decisions are
not A1. They are listed with reasons in `done.md`; several belong to D1 or to other classes.
