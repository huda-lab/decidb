# Query Diagnostics — Infeasible — open work

Current behavior and the accepted repair-tie policy are recorded in [`done.md`](done.md).

Nothing is open.

## Parked

Recorded so it is not lost; not to be built until the owner picks it up.

- **One row for an aggregate with no `per` in the expanded scope.** Expanded `DIAGNOSE` on `SUM(x) >= 500` over two
  rows prints two rows of amount 430, which reads like 860. That is D28 as written: a clause with no `per` key reports
  once per kept row (`expanded_row`). The owner's preferred direction is one row when the aggregate has no `per`. It
  changes what D28 and D33 say, so the decision goes first.
