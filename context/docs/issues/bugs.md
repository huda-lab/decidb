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

## BUG-27 — Two collations of one column share a key

- **Observed:** `FindOrCreateDecideKey` matches key elements with `Expression::Equals`, which ignores the collation,
  so `per t.g` over a NOCASE column and `by (t.g COLLATE noaccent)` share one key. With rows 'a' and 'A' the `per`
  class holds two `by` values, yet no scope check raises: the query answers `x = 1, 1, 1` instead of stopping with
  "per t.g does not determine t.g COLLATE noaccent". `KeyHolds` (BUG-26) reads the same equality. Repro:
  `SELECT id, x FROM (SELECT id, g COLLATE NOCASE AS g FROM (VALUES (1,'a'),(2,'A'),(3,'b')) v(id,g)) t DECIDE
  x(BOOL) SUCH THAT per t.g: sum(x) by (t.g COLLATE noaccent) <= 1 MAXIMIZE SUM(x)`.
- **Fix (to rule):** key elements compare as rows group on them, collation included, in both `FindOrCreateDecideKey`
  and `KeyHolds`.

## Limitations (not bugs)

None.

Resolved behavior is documented by its owning `done.md`.
