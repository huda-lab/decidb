# A1 (independent bounded decisions) — open work

What A1 does today is in `done.md`. The class definition is in `definition.md`. Each task is independent unless it says
otherwise. When one ships, its result moves to `done.md` and the task is deleted from here. How to run the checks, and the
batches that group tasks across S1, A1 and the shared engine, are in `../_harness/todo.md`; read its "How to work any
task" section first.

**A1 is built (2026-10-06).** The class definition is met; the tasks below are decisions and follow-ups, not missing shapes.
A review on 2026-10-07 found one wrong answer (an empty source with a bound the solver rejects before reading rows); it
is fixed (`ProveBounds` records the box the solver sees, and the declined shapes are tested in `DECLINED_CASES` in
`test_direct_a1.py`). A1-01, A1-02 and A1-04 shipped on 2026-10-09 and are in `done.md`. A1-06 and A1-07 shipped on the same day. A1-05
(re-stamp) remains.

## A1-03 — A wide column read back costs memory in proportion to the input

- **Problem.** The barrier holds every row until the check has run, so a parent that reads a wide column makes direct solve
  hold it: with a 512-byte payload at 1,000,000 rows the peak is 1.06 GiB where an unused payload costs 47 MiB.
- **Not known.** Whether a plan that evaluates the check in a first pass and joins the result back to the source (reading
  the source twice) beats holding the rows. `architecture/result_boundary.md` chose holding over a second read for S1; no
  measurement for A1's wide case exists. The constant-problem variant already streams.
- **Done when.** A measurement at 1,000,000 and 5,000,000 wide rows, and one recorded decision: keep the barrier or change
  the plan. Update the weak-spots paragraph in `done.md`. This is the same question as S1-05; run it once for both as
  `../_harness/todo.md` H-13.

## A1-05 — Re-stamp A1's verification

- **Problem.** The verification stamp at the top of `done.md` (date, commit, counts, and the sentence about "the 7
  existing tests listed in `todo.md` (A1-01)") and the one in `../_harness/done.md` (first paragraph, same sentence)
  describe the suite before the strict-bound fix: they list 6 expected failures and seven solver-only tests, which no
  longer exist. The doc text itself is current.
- **Plan.** Rerun all five checks (`../_harness/todo.md` lists them; `test/decide/run_direct_matrix.sh` does it) and rewrite both stamps (date, commit, counts). On 2026-10-09 the counts were: `make decide-test`, the
  `off` run and the serializer run 2,269 passed; the forced-HiGHS run 2,268 passed and 1 failure that needs Gurobi
  (`test_norm_combined_l1_l2_objective`); C++ `[decidb]` 1,227 assertions in 39 cases.
- **Done when.** No doc mentions the seven failing tests, the expected failures or A1-01, and the stamp matches a run you
  made.

## Out of the class, no task

`x <> k`, `x IN (...)`, objective `PER`, `AVG`/`MIN`/`MAX` objectives, entity or scalar decisions and coupled decisions are
not A1. They are listed with reasons in `done.md`; several belong to D1 or to other classes.
