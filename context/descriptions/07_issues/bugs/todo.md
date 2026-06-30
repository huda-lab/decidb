# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

Resolved bugs are moved to `done.md`.

---

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
