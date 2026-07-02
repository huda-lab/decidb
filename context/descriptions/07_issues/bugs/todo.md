# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

Resolved bugs are moved to `done.md`.

---

## Modulo (`%`) in a SUCH THAT coefficient crashes with INTERNAL

- **Location**: `ToSymbolicRecursive` (`src/decidb/symbolic/decide_symbolic.cpp`) — the operator-function switch
- **Discovered**: 2026-07-02 while building the slow-diagnostics test instances (needed data-driven coefficients)
- **Symptom**: A data-only modulo expression in a constraint coefficient position aborts with
  `INTERNAL Error: ToSymbolic: Unsupported operator function: %` instead of a friendly
  "unsupported expression" message (or folding it, since it contains no DECIDE variable).
- **Reproduction**:
  ```sql
  SELECT id, x FROM range(1,5) t(id)
  DECIDE x IS BOOLEAN
  SUCH THAT SUM(((id*7)%97)*x) <= 3;
  ```
- **Cause**: `ToSymbolic` handles a fixed set of operator functions and throws `InternalException`
  on any other. `%` is data-only here (no decide var), so the whole coefficient is foldable to a
  per-row constant — it should be evaluated as data, not symbolized. Computing the coefficient as a
  column in an inner subquery (`SELECT ((id*7)%97) AS c ...`) sidesteps it entirely.
- **Where to look next**: either (a) fold data-only subexpressions before symbolizing (general fix —
  any unsupported *data* operator would benefit), or (b) at minimum turn the `InternalException`
  into a friendly `InvalidInputException` naming the offending expression, per the user-output rule.

## SUCH THAT rejects composite (multi-column) PER

- **Location**: `src/planner/expression_binder/decide_constraints_binder.cpp` (PER binding)
- **Discovered**: 2026-07-01 while adding PER-group identity to infeasible diagnostics
- **Symptom**: `SUCH THAT SUM(x) >= 1 PER region, yr` fails to bind with `Binder Error: SUCH THAT clause does not support 'yr'(ExpressionClass::COLUMN_REF)`, even for a feasible query. Single-column PER works.
- **Reproduction**:
  ```sql
  SELECT id, x FROM (VALUES (1,'EU',2024),(2,'US',2025)) t(id, region, yr)
  DECIDE x IS BOOLEAN
  SUCH THAT SUM(x) >= 1 PER region, yr
  MAXIMIZE SUM(x);
  ```
- **Cause**: The constraint binder only consumes the first PER column. (The infeasible-diagnosis group-label path already renders composite keys as a `, `-joined tuple, so this is purely a binder gap.)
- **Where to look next**: Decide whether composite PER in `SUCH THAT` is in scope; if so, extend the constraints binder to accept the trailing PER columns the way the objective/symbolic path does.
