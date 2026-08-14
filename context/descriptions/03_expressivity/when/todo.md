# WHEN Keyword — Planned Features

## Known Feature Gaps

### Complex unparenthesized conditions remain restricted

Constraint-local `decide_when_condition` accepts a `c_expr`. A comparison there
is ambiguous with the constraint's own bound: `SUM(x) WHEN flag <= 20` already
means condition `flag`, bound `20`. Therefore an aggregate-local comparison
condition before a constraint bound must remain parenthesized. Objective WHEN
uses a distinct lexer token and admits one comparison between atomic operands,
because an objective has no trailing bound. No parsed-tree repair is involved.
Logical `NOT`, arithmetic, and compound conditions still require parentheses.

```sql
-- These fail without parentheses:
SUM(x * v) WHEN tier = 'high' <= 10
SUM(x * v) <= 12 WHEN NOT w
SUM(x * v) <= 12 WHEN a + b > 5

-- These work:
SUM(x * v) WHEN (tier = 'high') <= 10
MAXIMIZE SUM(x * v) WHEN tier = 'high'
SUM(x * v) <= 12 WHEN (NOT w)
SUM(x * v) <= 12 WHEN (a + b > 5)
```

**Practical guidance**: parenthesize every non-trivial condition, and always
parenthesize an aggregate-local comparison written before a constraint bound.
The parser hint (`MaybeAppendDecideWhenHint`) covers syntax errors near `NOT`,
arithmetic, and comparison tokens. A broader grammar would need an explicit
disambiguation rule rather than silently changing the established
`WHEN flag <= bound` meaning.

---

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

The second path used to be covered by the parsed-level symbolic translator, which
was deleted. The fix is to move the `CASE` check to where the constraint binder
classifies an unsupported expression class, so both spellings share one message —
`ExpressionClass::CASE` is internal jargon a SQL user cannot act on.

**Discovered**: 2026-08-14, verifying this document against the running binary
during the pipeline documentation restructure.
