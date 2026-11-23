# MODEL-1 — where the plan's form and the run-time count differ

`DECIDB_FORM_DIFF` over the Python suite at MODEL-1 (8,503 tests): each reducer helper whose form
(`ChooseHelperForms`) is not what `InlineReducerValueLinks` decided from the built model. A MIN or MAX never
differs. The deck's running example (`repro/deck_example.sql`) has no difference.

| Plan | Run time | Queries | What they are |
|---|---|---|---|
| a column | written in | 387 | a lone SUM or AVG that rows read, on tables of a few rows: `m·k <= m + 1 + k` holds only because the groups are tiny |
| a column | written in | 19 | a range of a whole timeline, or a range inside a one-sided ABS, on timelines of a few rows |
| written in | a column | 8 | a SUM or AVG that is a member of a MIN or MAX on its hard side: the count keeps it because each member is listed once per input row (design note § 1.3) |

Examples of the first row:

- `x <= 0.5 * sum(x) by (t.g) + 1`: every row of a group reads the group's sum.
- `ship <= D.maxShipmentShare * sum(ship) by (D.depotID)` (guide example 1).
- `per (): sum(x) by (t.g) + sum(x) by (t.h) <= 2 * t.cap`: a class split into variants, several rows per group.
- `per t.g: sum(x) by (t.g, t.h) + 2 * s <= 9`: one row per variant reads each sum once, which the count sees and
  the plan rule does not.

The eight of the last row:

- `max(sum(x + 1) by (t.product)) >= 15`
- `min(sum(abs(x - 3)) by (g)) <= 2`
- `per t.g: sum(per t.h: min(sum(when t.c: x) by (t.g, t.h, t.k)) by (t.g, t.h)) by (t.g) <= t.cap`
- `per t.g: min(avg(x) by (t.g, t.h)) by (t.g) <= 4`
- `per t.g: max(avg(x) by (t.g, t.h)) by (t.g) >= 4`
- `per t.g: max(sum(x) by (t.g, t.h)) by (t.g) >= 50`
- `MAXIMIZE max(avg(t.w * x) by (t.g))`
- `min(sum(when t.q: abs(max(...) by (t.w % 2))) by (t.g, t.h)) by (t.h) <= ...`

MODEL-2 changes the models of these queries: the first two rows gain a column and a definition row per group or
window; the last loses them.
