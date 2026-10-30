# Known bugs

## BUG-27 — Two collations of one column share a key

- **Observed:** `FindOrCreateDecideKey` matches key elements with `Expression::Equals`, which ignores the collation,
  so `per t.g` over a NOCASE column and `by (t.g COLLATE noaccent)` share one key. With rows 'a' and 'A' the `per`
  class holds two `by` values, yet no scope check raises: the query answers `x = 1, 1, 1` instead of stopping with
  "per t.g does not determine t.g COLLATE noaccent". `KeyHolds` (BUG-26) reads the same equality. Repro:
  `SELECT id, x FROM (SELECT id, g COLLATE NOCASE AS g FROM (VALUES (1,'a'),(2,'A'),(3,'b')) v(id,g)) t DECIDE
  x(BOOL) SUCH THAT per t.g: sum(x) by (t.g COLLATE noaccent) <= 1 MAXIMIZE SUM(x)`.
- **Fix (to rule):** key elements compare as rows group on them, collation included, in both `FindOrCreateDecideKey`
  and `KeyHolds`.

## Limitations (not bugs)

None.

Resolved behavior is documented by its owning `done.md`.
