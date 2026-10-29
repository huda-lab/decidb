# Known bugs

## BUG-25 — HiGHS overruns its time limit in two more loops (Q1 at the large tier)

- **Observed:** Q1 on HiGHS (`queries/q1_ilp_selection.sql`, large tier, 1M binaries, 8 rows, 300 s limit) runs past
  420 s in all three runs of the 2026-10-30 sweep and once more alone with the sweep's environment. Stack samples show
  one of two loops, depending on the run: presolve's probing in `HighsCliqueTable::extractCliques`
  (`HighsCliqueTable.cpp:860`), which pairs every binary with every non-binary column of a row (about 146k by 138k
  after presolve merges duplicate columns into integers; 215 s in one run), or the root heuristic
  `linesearchRounding` → `tryRoundedPoint` → `HighsDomain::conflictAnalysis` → `explainBoundChangeLeq`, at 330 s and
  400 s. Neither checks the time limit. The September sweep stopped at the root because HiGHS's default gap of 1e-4
  accepted 11357385338 against the optimum 11357408132; since the gap is 0 (2dbbffbdab) HiGHS keeps going and reaches
  the second loop.
- **Fix (owner's delegate, to confirm):** the BUG-23 pattern: a time check in each loop, the clique loop once per
  1,024 binaries, the rounding loop once per rounded point; past the limit each stops and returns what it has. Add
  both to `third_party/highs/README.md` and § 4 of `highs_upstream.md`.

## BUG-26 — A `per` key is checked against its own columns (Q8 at the large tier)

- **Observed:** `per o_orderpriority: sum(assign) by (o_orderpriority) >= 20` builds a scope check that
  `o_orderpriority` determines `o_orderpriority` (`BuildClauseScopeChecks`, `decide_canonicalizer.cpp:1811`). The
  check always passes yet sorts every input row in a window. Q8's four such checks take it from 0.7 s and 0.7 GiB
  (2026-09-09) to 3.7 s and 4.3 GiB (2026-10-30); Q1 and P4 carry them too.
- **Fix (owner's delegate, to confirm):** a key determines every expression over its own columns (reflexivity), so
  no scope check is built for one. The rule applies at every site that builds a `DETERMINES` check: a clause's values
  and `by` columns, and a reducer's body.

## Limitations (not bugs)

None.

Resolved behavior is documented by its owning `done.md`.
