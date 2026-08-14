# WHEN Keyword — Planned Features

## Known Feature Gaps

### `decide_when_condition` Grammar Is Restricted (`c_expr`)

The `decide_when_condition` non-terminal is `c_expr` — a restricted expression grammar that excludes:

- comparison operators (`=`, `<`, `>`, `<=`, `>=`, `<>`)
- logical `NOT`
- arithmetic (`+`, `-`)

Any of those tokens left unparenthesized inside a `WHEN` clause fails. Wrapping the condition in parentheses forces it through a different grammar production that supports the full set.

```sql
-- All of these FAIL to parse or misparse without parens:
SUM(x * v) WHEN tier = 'high' <= 10
SUM(x * v) <= 12 WHEN NOT w
SUM(x * v) <= 12 WHEN a + b > 5

-- All work when parenthesized:
SUM(x * v) WHEN (tier = 'high') <= 10
SUM(x * v) <= 12 WHEN (NOT w)
SUM(x * v) <= 12 WHEN (a + b > 5)
```

### Constraint vs. Objective Error Behavior Is Asymmetric

Unparenthesized WHEN conditions behave differently across constraint side, objective side, and condition shape. The error PHASE (parser vs. binder) and the resulting message text vary by combination. Empirically verified — pinned in `test/decide/tests/test_when_grammar.py`:

| WHEN shape | Constraint side | Objective side |
|---|---|---|
| `WHEN x = y` (comparison) | Parser error: `syntax error at or near "<="` (the `<=` token after the comparison is unparseable inside `c_expr`) | **Works** — `ReassociateObjectiveWhenComparison()` in `src/decidb/parsed/decide_grammar_repair.cpp` rewrites `(SUM(...) WHEN x) = y` into `SUM(...) WHEN (x = y)` |
| `WHEN NOT x` | Parser error: `syntax error at or near "NOT"` | Parser error: `syntax error at or near "NOT"` (reassociator only handles comparison-of-aggregate, not unary NOT) |
| `WHEN a + b > 5` | Parser error: `syntax error at or near "<="` | Binder error: `[MAXIMIZE\|MINIMIZE] clause does not support '...'(ExpressionClass::COMPARISON)` (parser succeeds via reassociator path but the resulting expression is not a valid objective component) |

```sql
-- All FAIL without parens:
SUM(x * value) WHEN tier = 'high' <= 10            -- constraint: parser
SUM(x * value) <= 12 WHEN NOT w                     -- both sides: parser
SUM(x * value) <= 12 WHEN a + b > 5                 -- constraint: parser
MAXIMIZE SUM(x * value) WHEN NOT w                  -- objective:  parser
MAXIMIZE SUM(x * value) WHEN a + b > 5              -- objective:  binder

-- All work when parenthesized:
SUM(x * value) WHEN (tier = 'high') <= 10
SUM(x * value) <= 12 WHEN (NOT w)
SUM(x * value) <= 12 WHEN (a + b > 5)
```

**Practical guidance**: always parenthesize non-trivial WHEN conditions. The objective-side reassociator works only on the simplest comparison-of-aggregate shape; everything else fails on either side.

**Potential fixes**: (a) extend `ReassociateObjectiveWhenComparison()` to cover constraints (would handle `WHEN x = y` on the constraint side); (b) widen the `decide_when_condition` grammar to admit `NOT`, comparisons, and arithmetic directly (requires `make grammar-build`) — this is the cleanest fix but touches the regenerated parser.

**Fix (c) shipped.** The parser error now carries an actionable parenthesization hint (`MaybeAppendDecideWhenHint`, `src/decidb/utility/decide_parse_hints.cpp`, called from `src/parser/parser.cpp`) — see `done.md` → "Actionable parser hint". This addresses the *messaging* half of the asymmetry; the underlying grammar restriction (needing parens at all) remains, so (a)/(b) are still the deeper fixes. The sentinel test in `test_when_grammar.py` stays valid because the hint is appended to — not a replacement for — the original `syntax error` text.

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
