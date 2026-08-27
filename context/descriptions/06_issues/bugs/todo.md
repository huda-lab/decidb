# Known Bugs — Open

Resolved behavior is documented by its owning `done.md`.

## Two levels of DECIDE nesting fail to plan

**Location**: raised as `Serialization Error: Cannot copy BoundSubqueryExpression`.

A DECIDE query nested inside another DECIDE clause plans and solves. A *third*
level — a DECIDE subquery inside a DECIDE subquery — parses, then fails when the
planner tries to copy the subquery expression:

```sql
SUCH THAT x >= 0 AND x <= 9
  AND SUM(x) <= (SELECT SUM(y) FROM t DECIDE y(INT) SUCH THAT y >= 0
        AND SUM(y) <= (SELECT SUM(z) FROM t DECIDE z(INT) SUCH THAT z >= 0 AND z <= 3
                       MAXIMIZE SUM(z))
      MAXIMIZE SUM(y))
MAXIMIZE SUM(x)
```

**What's wrong.** The failure is a planner limitation, not a parser one: it
reproduces identically with and without a trailing outer `WHEN`, so it is
independent of the lexer state work that made one level of nesting composable
with `WHEN`. The error names an internal class a SQL user cannot act on.

**Decision needed**: whether to support the second level, or to reject it at bind
time with a message in SQL terms. One level is enough for every shape currently
documented, so a clean rejection may be the right answer for this release.

**Test**: the query above, asserting whichever contract is chosen; the one-level
cases in `test/decide/tests/test_nested_decide.py` must stay green either way.

**Discovered**: 2026-08-27, verifying the nested-DECIDE lexer fix.
