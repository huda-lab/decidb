# WHEN Keyword — Planned Features

## A `CASE` outside a reducer gets the generic binder message, not the friendly one

`ValidateSumArgumentInternal` produces the intended message — naming postfix
`WHEN`, `PER`, and CTE pre-computation as the alternatives — but only for a `CASE`
**inside** a reducer. A `CASE` elsewhere in a constraint falls through to DuckDB's
generic wording.

```sql
-- friendly (inside a reducer)
SUCH THAT SUM(x * (CASE WHEN a>0 THEN 1 ELSE 2 END)) <= 5
  → CASE expressions are not supported inside DECIDE constraints or objectives.
    Use postfix WHEN … PER … or a CTE/subquery …

-- generic (outside a reducer)
SUCH THAT x + (CASE WHEN a>0 THEN 1 ELSE 2 END) <= 5
  → SUCH THAT clause does not support 'CASE  WHEN ((a > 0)) THEN (1) ELSE 2 END'
    (ExpressionClass::CASE)
```

The fix is to move the `CASE` check to where the constraint binder
classifies an unsupported expression class, so both spellings share one message —
`ExpressionClass::CASE` is internal jargon a SQL user cannot act on.

**Discovered**: 2026-08-14, verifying this document against the running binary
during the pipeline documentation restructure.
