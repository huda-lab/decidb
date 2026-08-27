# ABS Linearization Test Coverage — Done

Tests live in:
- `test/decide/tests/test_abs_linearization.py` — primary ABS coverage
- `test/decide/tests/test_per_interactions.py` — ABS in aggregate constraint with PER (per-group ABS aux)

The ABS linearization is performed by `DecideOptimizer::RewriteAbs`. For each
`ABS(expr)` referencing a DECIDE variable, an auxiliary REAL variable `d` is
introduced with the lower envelope `d >= expr` and `d >= -expr`. ABS occurrences
that don't naturally pin `d` to `|expr|` (MAXIMIZE objective, or constraint
shapes that don't upper-bound `d`) additionally get a Big-M sign-indicator
binary `y` and the upper envelope `d <= expr + 2M(1-y)` and `d <= -expr + 2M*y`.
The classifier `TagAbsConstraintsForBigM` runs before `RewriteAbs` and tags
Path-B occurrences, per ABS *occurrence* and by the sign it carries in
`LHS - RHS` rather than by the side it was written on. See
`03_expressivity/sql_functions/done.md` for the full Path-A / Path-B
classification.

## Scenarios covered

- **Sound directions** (oracle-verified in `test_abs_linearization.py`): ABS in objective (basic, with WHEN, with PER on a separate SUM); per-row `ABS(expr) <= K`; aggregate `SUM(ABS(expr)) <= K` (plain and WHEN-masked aux sum); multiple ABS terms in one expression; mixed BOOL + REAL variables (`test_abs_mixed_vars`); ABS with no DECIDE variable (passthrough, no oracle needed).
- **PER interaction**: ABS in aggregate constraint with PER (per-group aux) — `test_per_interactions.py::test_per_abs_aggregate`, oracle-verified.
- **Hard directions (Big-M)** — oracle-verified per-row `>=` and `=`, aggregate
  `SUM(ABS) >= K`, `MIN(ABS) >= K`, `MAX(ABS) >= K`, `BETWEEN`, and ABS on both
  sides of a comparison. The stress corpus C33–C37 supplies an additional model-shape
  smoke check.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| ABS | BOOL | ✓ |
| ABS | INT | ✓ |
| ABS | REAL | ✓ |
| ABS | Multiple variable types | ✓ |
| ABS (objective) | WHEN | ✓ |
| ABS (objective) | PER (on a sibling constraint) | ✓ |
| ABS (per-row constraint, sound direction) | — | ✓ |
| ABS (aggregate constraint, sound direction) | — | ✓ |
| ABS (aggregate constraint) | WHEN (auxiliary-variable mask propagation) | ✓ |
| ABS (aggregate constraint) | PER (per-group aux partitioning) | ✓ |
| ABS (per-row constraint, hard direction `>=`/`=`/BETWEEN) | Big-M sign-indicator | ✓ (oracle + stress C33–C37) |
| ABS (aggregate constraint, hard direction `SUM(ABS)>=K` / `MIN(ABS)>=K` / `MAX(ABS)>=K`) | Big-M on each aux | ✓ (oracle + stress C35–C36) |
