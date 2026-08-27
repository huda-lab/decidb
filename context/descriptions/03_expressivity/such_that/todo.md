# SUCH THAT Clause — Planned Features

## NULL Coefficient Handling

**Priority: Low — requires design decision**

Currently, NULL values in constraint or objective coefficients (e.g., `SUM(x * weight)` where `weight` is NULL for some rows) produce an error:

> *"DECIDE constraint coefficient returned NULL at row N. NULL values are not allowed in optimization coefficients. Use COALESCE() to handle NULLs or filter them with WHERE clause."*

**Open question**: Should DeciDB silently treat NULL coefficients as 0 (matching SQL `SUM()` semantics where NULLs are ignored), or is requiring explicit `COALESCE()` the right design?

**Arguments for treating as 0**: SQL semantics — `SUM()` ignores NULLs. Users expect DeciDB to extend SQL naturally.

**Arguments for current behavior (error)**: NULLs in optimization coefficients are almost certainly a data quality issue. Silent coercion to 0 could hide bugs. The current error message helpfully suggests `COALESCE()`, making the user's intent explicit.

---
