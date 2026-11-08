# Query Diagnostics — Infeasible — open work

Current behavior and the accepted repair-tie policy are recorded in [`done.md`](done.md).

## Parked

Recorded so it is not lost; not to be built until the owner picks it up.

- **One row for an aggregate with no `per` in the expanded scope.** Expanded `DIAGNOSE` on `SUM(x) >= 500` over two
  rows prints two rows of amount 430, which reads like 860. That is D28 as written: a clause with no `per` key reports
  once per kept row (`expanded_row`). The owner's preferred direction is one row when the aggregate has no `per`. It
  changes what D28 and D33 say, so the decision goes first.

## BLOAT-12 — Bloat in the diagnosis code and its tests

- **Problem.** The diagnosis code of every outcome (`src/decidb/diagnostics/`; about 2,500 lines) and its tests
  (`test_query_diagnostics_*.py`, `test_diagnos*.py`, `test_diagnose_trigger.py`, `_diagnose_relation.py`,
  `_diagnostic_invariants.py`; about 5,800 lines) have not been read for bloat.
- **Change.** Remove the bloat by `../../direct_solve/framework/todo.md` § How to work any task (Removing bloat), in
  these files only.
- **Done when.** `make decide-test` and `build/release/test/unittest "[decidb]"` pass with no answer, error text or
  golden changed, and the report lists each removal with its evidence and the lines and `lizard` figures
  before and after.
