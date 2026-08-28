# Known Bugs — Open

Resolved behavior is documented by its owning `done.md`.

## A matrix coefficient at or below 1e-9 crashes the HiGHS backend

**Location**: `HighsSession::Load` in `src/decidb/naive/deterministic_naive.cpp` — the
`highs.passModel(lp)` status check.

**What's wrong.** HiGHS silently drops matrix entries whose magnitude is at or below its
`small_matrix_value` option (default `1e-9`) and returns `kWarning` from `passModel` to
say so. Our loader treats every non-`kOk` status as fatal and raises an
`InternalException`, so the user gets a stack trace and an "internal error" page instead
of a result. Reproduced on an ordinary query with no DECIDE construct involved:

```sql
SELECT id, x FROM (VALUES (1,0.000000001),(2,0.000000001)) t(id,w)
DECIDE x(REAL) SUCH THAT x >= 0 AND x <= 5 AND SUM(w * x) <= 1 MAXIMIZE SUM(x);
-- INTERNAL Error: Failed to pass model to HiGHS: status 1
```

**Why it matters.** A tiny coefficient is ordinary data — a rate, a per-unit cost in the
wrong unit — not a malformed query. Gurobi accepts the same model. So the query works or
crashes depending on which solver is installed, and the crash is presented as an internal
error rather than anything the user can act on.

**It also reaches diagnosis.** The infeasible engine's scale-normalized tier-1 weights are
`ref / rms(Aᵢ)`, so a row whose coefficients are around `1e9` gets a weight around `1e-9`.
That weight becomes a matrix entry in the stage-2 budget row, and the same crash follows
from a query whose own coefficients are all perfectly ordinary. `DIAGNOSE` no longer
propagates it (the terminal catches a failing repair solve and reports "the solver could
not analyse the repair model"), so today this shows up as a diagnosis that declines rather
than a crash — but the underlying loader is still wrong.

**Fix shape**: distinguish `kWarning` from `kError` at the `passModel` call. A dropped
sub-tolerance coefficient is a warning HiGHS expects callers to proceed through. Decide
separately whether to surface it to the user at all — dropping a `1e-9` coefficient
changes the model, so a one-line notice naming the column may be worth more than silence.

**Test**: the query above, on both backends, asserting a result rather than an error; plus
a diagnosis over a row with `1e9`-scale coefficients asserting a named repair rather than
"could not analyse".

**Discovered**: 2026-08-28, by the scale sweep in
`test/decide/tests/test_diagnosis_repair_sweep.py` (`heavy-row@1000`).
