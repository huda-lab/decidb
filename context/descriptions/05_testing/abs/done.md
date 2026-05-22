# ABS Linearization Test Coverage — Done

Tests live in:
- `test/decide/tests/test_abs_linearization.py` (9 tests)
- `test/decide/tests/test_per_interactions.py` — ABS in aggregate constraint with PER (per-group ABS aux)

The ABS linearization is performed by `DecideOptimizer::RewriteAbs`. For each
`ABS(expr)` referencing a DECIDE variable, an auxiliary REAL variable `d` is
introduced with the lower envelope `d >= expr` and `d >= -expr`. ABS occurrences
that don't naturally pin `d` to `|expr|` (MAXIMIZE objective, or constraint
shapes that don't upper-bound `d`) additionally get a Big-M sign-indicator
binary `y` and the upper envelope `d <= expr + 2M(1-y)` and `d <= -expr + 2M*y`.
The classifier `TagAbsConstraintsForBigM` runs before `RewriteAbs` and tags
Path-B occurrences. See `03_expressivity/sql_functions/done.md` for the full
Path-A / Path-B classification.

## Scenarios covered

| Scenario | Where | Oracle |
|----------|-------|--------|
| ABS in objective (basic) | `test_abs_linearization.py` | ✓ |
| ABS in objective with WHEN | `test_abs_linearization.py::test_abs_objective_with_when` | ✓ |
| ABS in objective with PER (PER on separate SUM) | `test_abs_linearization.py::test_abs_objective_with_per` | ✓ |
| ABS in per-row constraint, sound direction (`ABS(expr) <= K`) | `test_abs_linearization.py` | ✓ |
| ABS in aggregate constraint, sound direction (`SUM(ABS(expr)) <= K`) | `test_abs_linearization.py` | ✓ |
| ABS in aggregate constraint with WHEN (WHEN-masked aux sum) | `test_abs_linearization.py::test_abs_constraint_aggregate_with_when` | ✓ |
| ABS in aggregate constraint with PER (per-group aux) | `test_per_interactions.py::test_per_abs_aggregate` | ✓ |
| Multiple ABS terms in same expression | `test_abs_linearization.py` | ✓ |
| ABS with no decide variable (passthrough) | `test_abs_linearization.py` | — |
| ABS with mixed BOOLEAN + REAL variables | `test_abs_linearization.py::test_abs_mixed_vars` | ✓ |
| Per-row ABS hard direction (`ABS(expr) >= K`) — Big-M | `stress_queries/01_constraints.sql` C33 | smoke |
| ABS equality (`ABS(expr) = K`) — Big-M | `stress_queries/01_constraints.sql` C34 | smoke |
| Aggregate hard via easy-MIN strip (`MIN(ABS) >= K`) — per-row Big-M | `stress_queries/01_constraints.sql` C35 | smoke |
| Aggregate hard direction (`SUM(ABS) >= K`) — Big-M on each aux | `stress_queries/01_constraints.sql` C36 | smoke |
| ABS in BETWEEN (`ABS(expr) BETWEEN a AND b`) — Big-M | `stress_queries/01_constraints.sql` C37 | smoke |

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| ABS | BOOLEAN | ✓ |
| ABS | INTEGER | ✓ |
| ABS | REAL | ✓ |
| ABS | Multiple variable types | ✓ |
| ABS (objective) | WHEN | ✓ |
| ABS (objective) | PER (on a sibling constraint) | ✓ |
| ABS (per-row constraint, sound direction) | — | ✓ |
| ABS (aggregate constraint, sound direction) | — | ✓ |
| ABS (aggregate constraint) | WHEN (auxiliary-variable mask propagation) | ✓ |
| ABS (aggregate constraint) | PER (per-group aux partitioning) | ✓ |
| ABS (per-row constraint, hard direction `>=`/`=`/`<>`/BETWEEN) | Big-M sign-indicator | ✓ (smoke via stress C33–C37; oracle test gap, see todo) |
| ABS (aggregate constraint, hard direction `SUM(ABS)>=K` / `MIN(ABS)>=K`) | Big-M on each aux | ✓ (smoke via stress C35–C36; oracle test gap, see todo) |
