# Query Diagnostics — Infeasible — open work

Current behavior and the accepted repair-tie policy are recorded in [`done.md`](done.md).

## INF-5 — Three label formatters no test reaches

- **Problem.** `FormatLhs`, `FormatSumLhs` and `FormatFoldedSumLhs` (`src/decidb/diagnostics/decide_diagnostic_render.cpp`)
  run only when `SourceAwareLhs` meets a row with no source clause or an empty `canonical_lhs`. A trace over the 320
  tests of `test_query_diagnostics_relation.py` reached `SourceAwareLhs` 351 times and never took that branch, and a
  mutation of `FormatFoldedSumLhs`'s data-varying branch failed no test there.
- **Change.** Establish whether any query reaches the no-source branch (every DIAGNOSE mode, the unit tests). If none
  does, the three formatters, the branch and the tests that reach only them go in one change; if one does, a test
  reaches it.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "diagnos"` and `build/release/test/unittest "[decidb]"` pass.

## Parked

Recorded so it is not lost; not to be built until the owner picks it up.

- **One row for an aggregate with no `per` in the expanded scope.** Expanded `DIAGNOSE` on `SUM(x) >= 500` over two
  rows prints two rows of amount 430, which reads like 860. That is D28 as written: a clause with no `per` key reports
  once per kept row (`expanded_row`). The owner's preferred direction is one row when the aggregate has no `per`. It
  changes what D28 and D33 say, so the decision goes first.
