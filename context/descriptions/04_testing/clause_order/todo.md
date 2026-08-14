# Clause-Order Test Coverage — Todo

No open gaps. Every scenario listed in `done.md` is covered, including the source
shapes and the `WHEN` / `PER` compositions that were open when this area was
created on 2026-08-08.

The constraints-and-objective slot of the split order is the same non-terminal
the single-block order uses, and the declaration slot is reassembled into one
`PGDecideClause` before binding — so a feature that works in the single-block
order works in the split order by construction. The tests in `done.md` verify
that claim at the four points where it could plausibly fail (subquery/CTE source,
three-table join, `WHEN`, `PER`) rather than at every feature.
